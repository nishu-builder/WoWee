#include "core/godview_replay.hpp"

#include "core/coordinates.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/renderer.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace wowee {
namespace core {

namespace {

uint32_t makeAppearanceBytes(uint64_t guid) {
    uint8_t skin = static_cast<uint8_t>((guid >> 0) & 0x03);
    uint8_t face = static_cast<uint8_t>((guid >> 8) & 0x03);
    uint8_t hair = static_cast<uint8_t>((guid >> 16) & 0x05);
    uint8_t hairColor = static_cast<uint8_t>((guid >> 24) & 0x04);
    return static_cast<uint32_t>(skin) |
           (static_cast<uint32_t>(face) << 8) |
           (static_cast<uint32_t>(hair) << 16) |
           (static_cast<uint32_t>(hairColor) << 24);
}

void setFieldIfValid(game::Entity& entity, game::UF field, uint32_t value) {
    uint16_t idx = game::fieldIndex(field);
    if (idx != 0xFFFF) entity.setField(idx, value);
}

} // namespace

bool GodviewReplay::load(const std::string& path, std::string& error) {
    activeGuids_.clear();
    lastMoving_.clear();

    if (!recording_.load(path, error)) {
        return false;
    }

    mapId_ = recording_.firstSnapshot().map;
    for (const auto& snapshot : recording_.snapshots()) {
        if (!snapshot.players.empty()) {
            mapId_ = snapshot.map;
            break;
        }
    }

    startMs_ = recording_.startMsForMap(mapId_);
    endMs_ = recording_.endMsForMap(mapId_);
    currentMs_ = static_cast<double>(startMs_);
    speed_ = 1.0f;
    paused_ = false;

    LOG_INFO("Godview replay active map: ", mapId_,
             " snapshots=", recording_.snapshotCountForMap(mapId_),
             " ms=", startMs_, "-", endMs_);
    return true;
}

const GodviewReplay::Snapshot& GodviewReplay::firstSnapshot() const {
    if (const Snapshot* snapshot = recording_.firstSnapshotForMap(mapId_)) {
        return *snapshot;
    }
    return recording_.firstSnapshot();
}

const GodviewReplay::Player* GodviewReplay::firstPlayer() const {
    return recording_.firstPlayerForMap(mapId_);
}

void GodviewReplay::start() {
    setCurrentMs(static_cast<double>(startMs_));
    paused_ = false;
    activeGuids_.clear();
    lastMoving_.clear();
}

void GodviewReplay::setCurrentMs(double value) {
    if (recording_.empty()) {
        currentMs_ = 0.0;
        return;
    }
    currentMs_ = std::clamp(value, static_cast<double>(startMs_), static_cast<double>(endMs_));
}

void GodviewReplay::applyGameState(game::GameHandler& gameHandler,
                                   EntitySpawner& entitySpawner,
                                   const std::vector<InterpolatedPlayer>& players) {
    auto& entities = gameHandler.getEntityManager();
    std::unordered_set<uint64_t> active;
    active.reserve(players.size());

    for (const auto& sampled : players) {
        const Player& player = sampled.player;
        active.insert(player.guid);

        std::shared_ptr<game::Player> entity;
        auto existing = entities.getEntity(player.guid);
        if (existing && existing->getType() == game::ObjectType::PLAYER) {
            entity = std::static_pointer_cast<game::Player>(existing);
        } else {
            entity = std::make_shared<game::Player>(player.guid);
            entities.addEntity(player.guid, entity);
        }

        glm::vec3 canonical = coords::serverToCanonical(glm::vec3(player.x, player.y, player.z));
        float canonicalYaw = coords::serverToCanonicalYaw(player.orientation);
        entity->setName(player.name.empty() ? "Player" : player.name);
        entity->setLevel(player.level);
        entity->setHealth(player.hp);
        entity->setMaxHealth(std::max<uint32_t>(1, player.maxHp));
        entity->setPosition(canonical.x, canonical.y, canonical.z, canonicalYaw);

        const uint32_t bytes0 = static_cast<uint32_t>(player.race) |
                                (static_cast<uint32_t>(player.playerClass) << 8) |
                                (static_cast<uint32_t>(player.gender) << 16);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_BYTES_0, bytes0);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_HEALTH, player.hp);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_MAXHEALTH, std::max<uint32_t>(1, player.maxHp));
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_LEVEL, player.level);
        if (player.targetGuid) {
            setFieldIfValid(*entity, game::UF::UNIT_FIELD_TARGET_LO, static_cast<uint32_t>(*player.targetGuid & 0xFFFFFFFFu));
            setFieldIfValid(*entity, game::UF::UNIT_FIELD_TARGET_HI, static_cast<uint32_t>(*player.targetGuid >> 32));
        } else {
            setFieldIfValid(*entity, game::UF::UNIT_FIELD_TARGET_LO, 0);
            setFieldIfValid(*entity, game::UF::UNIT_FIELD_TARGET_HI, 0);
        }

        if (!entitySpawner.isPlayerSpawned(player.guid) && !entitySpawner.isPlayerPending(player.guid)) {
            entitySpawner.queuePlayerSpawn(player.guid,
                                           player.race,
                                           player.gender,
                                           makeAppearanceBytes(player.guid),
                                           static_cast<uint8_t>((player.guid >> 32) & 0x03),
                                           canonical.x,
                                           canonical.y,
                                           canonical.z,
                                           canonicalYaw);
        }
    }

    despawnMissing(gameHandler, entitySpawner, active);
    activeGuids_ = std::move(active);
}

void GodviewReplay::despawnMissing(game::GameHandler& gameHandler,
                                   EntitySpawner& entitySpawner,
                                   const std::unordered_set<uint64_t>& activeGuids) {
    std::vector<uint64_t> toDespawn;
    for (uint64_t guid : activeGuids_) {
        if (!activeGuids.count(guid)) toDespawn.push_back(guid);
    }

    for (uint64_t guid : toDespawn) {
        entitySpawner.despawnPlayer(guid);
        gameHandler.getEntityManager().removeEntity(guid);
        lastMoving_.erase(guid);
    }
}

void GodviewReplay::update(float deltaTime,
                           game::GameHandler& gameHandler,
                           EntitySpawner& entitySpawner,
                           rendering::Renderer& renderer) {
    if (!paused_ && !recording_.empty()) {
        setCurrentMs(currentMs_ + static_cast<double>(deltaTime) * 1000.0 * static_cast<double>(speed_));
        if (currentMs_ >= static_cast<double>(endMs_)) {
            paused_ = true;
        }
    }

    auto players = recording_.samplePlayers(currentMs_, mapId_);
    applyGameState(gameHandler, entitySpawner, players);
    syncRender(gameHandler, entitySpawner, renderer);
}

void GodviewReplay::syncRender(game::GameHandler& gameHandler,
                               EntitySpawner& entitySpawner,
                               rendering::Renderer& renderer) {
    (void)gameHandler;
    auto* charRenderer = renderer.getCharacterRenderer();
    if (!charRenderer) return;

    auto players = recording_.samplePlayers(currentMs_, mapId_);
    for (const auto& sampled : players) {
        const Player& player = sampled.player;
        uint32_t instanceId = entitySpawner.getPlayerInstanceId(player.guid);
        if (instanceId == 0) continue;

        glm::vec3 canonical = coords::serverToCanonical(glm::vec3(player.x, player.y, player.z));
        glm::vec3 renderPos = coords::canonicalToRender(canonical);
        float renderYaw = coords::serverToCanonicalYaw(player.orientation) + glm::radians(90.0f);
        charRenderer->setInstancePosition(instanceId, renderPos);
        charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, 0.0f, renderYaw));

        const bool moving = sampled.moving;
        auto lastIt = lastMoving_.find(player.guid);
        bool stateChanged = (lastIt == lastMoving_.end() || lastIt->second != moving);
        if (stateChanged) {
            uint32_t anim = moving ? rendering::anim::RUN
                                   : (player.combat ? rendering::anim::READY_UNARMED
                                                    : rendering::anim::STAND);
            if (!charRenderer->hasAnimation(instanceId, anim)) {
                anim = moving ? rendering::anim::RUN : rendering::anim::STAND;
            }
            charRenderer->playAnimation(instanceId, anim, true);
            lastMoving_[player.guid] = moving;
        }
    }
}

void GodviewReplay::handleKeyDown(const SDL_KeyboardEvent& event) {
    if (event.repeat != 0) return;
    switch (event.keysym.scancode) {
        case SDL_SCANCODE_SPACE:
            paused_ = !paused_;
            break;
        case SDL_SCANCODE_LEFT:
            setCurrentMs(currentMs_ - ((event.keysym.mod & KMOD_SHIFT) ? 30000.0 : 5000.0));
            paused_ = true;
            break;
        case SDL_SCANCODE_RIGHT:
            setCurrentMs(currentMs_ + ((event.keysym.mod & KMOD_SHIFT) ? 30000.0 : 5000.0));
            paused_ = true;
            break;
        case SDL_SCANCODE_HOME:
            setCurrentMs(static_cast<double>(startMs_));
            paused_ = true;
            break;
        case SDL_SCANCODE_END:
            setCurrentMs(static_cast<double>(endMs_));
            paused_ = true;
            break;
        case SDL_SCANCODE_LEFTBRACKET:
            speed_ = std::max(0.125f, speed_ * 0.5f);
            break;
        case SDL_SCANCODE_RIGHTBRACKET:
            speed_ = std::min(16.0f, speed_ * 2.0f);
            break;
        default:
            break;
    }
}

void GodviewReplay::renderOverlay() {
    if (recording_.empty()) return;

    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Replay", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(recording_.path().c_str());
    if (ImGui::Button(paused_ ? "Play" : "Pause")) {
        paused_ = !paused_;
    }
    ImGui::SameLine();
    if (ImGui::Button("Start")) {
        setCurrentMs(static_cast<double>(startMs_));
        paused_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("End")) {
        setCurrentMs(static_cast<double>(endMs_));
        paused_ = true;
    }

    const float durationSeconds = static_cast<float>(
        std::max<uint64_t>(1, endMs_ > startMs_ ? endMs_ - startMs_ : 1) / 1000.0);
    float positionSeconds = static_cast<float>((currentMs_ - static_cast<double>(startMs_)) / 1000.0);
    if (ImGui::SliderFloat("Time", &positionSeconds, 0.0f, durationSeconds, "%.1fs")) {
        setCurrentMs(static_cast<double>(startMs_) + static_cast<double>(positionSeconds) * 1000.0);
        paused_ = true;
    }
    if (ImGui::SliderFloat("Speed", &speed_, 0.125f, 16.0f, "%.3gx", ImGuiSliderFlags_Logarithmic)) {
        speed_ = std::clamp(speed_, 0.125f, 16.0f);
    }

    auto pair = recording_.findSnapshotPair(currentMs_, mapId_);
    const auto& snapshots = recording_.snapshots();
    size_t players = pair.valid ? snapshots[pair.prev].players.size() : 0;
    ImGui::Text("Map %u  Snapshots %zu  Players %zu",
                mapId_,
                recording_.snapshotCountForMap(mapId_),
                players);
    ImGui::Text("ms %.0f / %llu", currentMs_ - static_cast<double>(startMs_),
                static_cast<unsigned long long>(endMs_ - startMs_));
    ImGui::TextUnformatted("Space pause, arrows scrub, brackets speed");

    ImGui::End();
}

} // namespace core
} // namespace wowee
