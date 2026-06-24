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
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace wowee {
namespace core {

namespace {

uint32_t makeAppearanceBytes(uint8_t skin, uint8_t face, uint8_t hair, uint8_t hairColor) {
    return static_cast<uint32_t>(skin) |
           (static_cast<uint32_t>(face) << 8) |
           (static_cast<uint32_t>(hair) << 16) |
           (static_cast<uint32_t>(hairColor) << 24);
}

uint32_t makeAppearanceBytes(uint64_t guid) {
    uint8_t skin = static_cast<uint8_t>((guid >> 0) & 0x03);
    uint8_t face = static_cast<uint8_t>((guid >> 8) & 0x03);
    uint8_t hair = static_cast<uint8_t>((guid >> 16) & 0x05);
    uint8_t hairColor = static_cast<uint8_t>((guid >> 24) & 0x04);
    return makeAppearanceBytes(skin, face, hair, hairColor);
}

bool isPlayableRace(uint8_t raceId) {
    switch (static_cast<game::Race>(raceId)) {
        case game::Race::HUMAN:
        case game::Race::ORC:
        case game::Race::DWARF:
        case game::Race::NIGHT_ELF:
        case game::Race::UNDEAD:
        case game::Race::TAUREN:
        case game::Race::GNOME:
        case game::Race::TROLL:
        case game::Race::BLOOD_ELF:
        case game::Race::DRAENEI:
            return true;
        default:
            return false;
    }
}

bool hasDisplayEquipment(const EntitySpawner::HumanoidDisplayAppearance& appearance) {
    return std::any_of(appearance.equipmentDisplayIds.begin(),
                       appearance.equipmentDisplayIds.end(),
                       [](uint32_t displayId) { return displayId != 0; });
}

void buildDisplayEquipmentArrays(const EntitySpawner::HumanoidDisplayAppearance& appearance,
                                 std::array<uint32_t, 19>& displayInfoIds,
                                 std::array<uint8_t, 19>& inventoryTypes) {
    displayInfoIds.fill(0);
    inventoryTypes.fill(0);

    static constexpr std::array<uint8_t, 11> kPlayerSlots = {
        0,  // helm
        2,  // shoulder
        3,  // shirt
        4,  // chest
        5,  // belt
        6,  // legs
        7,  // feet
        8,  // wrist
        9,  // hands
        18, // tabard
        14  // cape
    };
    static constexpr std::array<uint8_t, 11> kInventoryTypes = {
        1,  // head
        3,  // shoulder
        4,  // body
        5,  // chest
        6,  // waist
        7,  // legs
        8,  // feet
        9,  // wrists
        10, // hands
        19, // tabard
        16  // cloak
    };

    for (size_t i = 0; i < appearance.equipmentDisplayIds.size(); i++) {
        uint32_t displayId = appearance.equipmentDisplayIds[i];
        if (displayId == 0) continue;
        uint8_t slot = kPlayerSlots[i];
        displayInfoIds[slot] = displayId;
        inventoryTypes[slot] = kInventoryTypes[i];
    }
}

void setFieldIfValid(game::Entity& entity, game::UF field, uint32_t value) {
    uint16_t idx = game::fieldIndex(field);
    if (idx != 0xFFFF) entity.setField(idx, value);
}

void setTargetFields(game::Entity& entity, const std::optional<uint64_t>& targetGuid) {
    if (targetGuid) {
        setFieldIfValid(entity, game::UF::UNIT_FIELD_TARGET_LO, static_cast<uint32_t>(*targetGuid & 0xFFFFFFFFu));
        setFieldIfValid(entity, game::UF::UNIT_FIELD_TARGET_HI, static_cast<uint32_t>(*targetGuid >> 32));
    } else {
        setFieldIfValid(entity, game::UF::UNIT_FIELD_TARGET_LO, 0);
        setFieldIfValid(entity, game::UF::UNIT_FIELD_TARGET_HI, 0);
    }
}

size_t equipmentHash(const GodviewRecording::Player& player) {
    size_t hash = 1469598103934665603ull;
    auto mix = [&](uint64_t value) {
        hash ^= static_cast<size_t>(value);
        hash *= 1099511628211ull;
    };

    for (const auto& equipment : player.equipment) {
        mix(equipment.slot);
        mix(equipment.itemId);
        mix(equipment.displayId);
        mix(equipment.inventoryType);
        mix(equipment.itemClass);
        mix(equipment.subclass);
    }

    return hash;
}

size_t displayEquipmentHash(const EntitySpawner::HumanoidDisplayAppearance& appearance) {
    size_t hash = 1099511628211ull;
    for (size_t i = 0; i < appearance.equipmentDisplayIds.size(); i++) {
        hash ^= static_cast<size_t>(i);
        hash *= 1099511628211ull;
        hash ^= static_cast<size_t>(appearance.equipmentDisplayIds[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

void buildEquipmentArrays(const GodviewRecording::Player& player,
                          std::array<uint32_t, 19>& displayInfoIds,
                          std::array<uint8_t, 19>& inventoryTypes) {
    displayInfoIds.fill(0);
    inventoryTypes.fill(0);
    for (const auto& equipment : player.equipment) {
        if (equipment.slot >= displayInfoIds.size()) continue;
        displayInfoIds[equipment.slot] = equipment.displayId;
        inventoryTypes[equipment.slot] = equipment.inventoryType;
    }
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool playerMatchesFocusQuery(const GodviewRecording::Player& player,
                             const std::string& rawQuery,
                             const std::string& loweredQuery,
                             bool allowSubstring) {
    if (rawQuery == std::to_string(player.guid) || player.rawGuid == rawQuery) {
        return true;
    }

    std::string loweredName = lowerAscii(player.name);
    if (allowSubstring) {
        return loweredName.find(loweredQuery) != std::string::npos;
    }
    return loweredName == loweredQuery;
}

glm::vec3 toRenderPosition(float x, float y, float z) {
    glm::vec3 canonical = coords::serverToCanonical(glm::vec3(x, y, z));
    return coords::canonicalToRender(canonical);
}

} // namespace

bool GodviewReplay::load(const std::string& path, std::string& error) {
    activePlayerGuids_.clear();
    activeCreatureGuids_.clear();
    lastMoving_.clear();
    lastCreatureMoving_.clear();
    lastPlayerEquipmentHash_.clear();

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
    cameraFollowEnabled_ = false;
    focusedPlayerGuid_ = 0;

    LOG_INFO("Godview replay active map: ", mapId_,
             " snapshots=", recording_.snapshotCountForMap(mapId_),
             " creatures=", recording_.creatureCountForMap(mapId_),
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
    activePlayerGuids_.clear();
    activeCreatureGuids_.clear();
    lastMoving_.clear();
    lastCreatureMoving_.clear();
    lastPlayerEquipmentHash_.clear();
}

void GodviewReplay::setCurrentMs(double value) {
    if (recording_.empty()) {
        currentMs_ = 0.0;
        return;
    }
    currentMs_ = std::clamp(value, static_cast<double>(startMs_), static_cast<double>(endMs_));
}

void GodviewReplay::setCameraFollowEnabled(bool enabled) {
    cameraFollowEnabled_ = enabled;
    if (cameraFollowEnabled_ && focusedPlayerGuid_ == 0) {
        focusNextPlayer(1);
    }
}

void GodviewReplay::focusNextPlayer(int direction) {
    auto players = recording_.samplePlayers(currentMs_, mapId_);
    if (players.empty()) {
        focusedPlayerGuid_ = 0;
        cameraFollowEnabled_ = false;
        return;
    }

    size_t index = 0;
    bool foundCurrent = false;
    for (size_t i = 0; i < players.size(); ++i) {
        if (players[i].player.guid == focusedPlayerGuid_) {
            index = i;
            foundCurrent = true;
            break;
        }
    }

    if (foundCurrent) {
        const int count = static_cast<int>(players.size());
        int next = static_cast<int>(index) + (direction < 0 ? -1 : 1);
        next = (next % count + count) % count;
        index = static_cast<size_t>(next);
    } else if (direction < 0) {
        index = players.size() - 1;
    }

    focusedPlayerGuid_ = players[index].player.guid;
    cameraFollowEnabled_ = true;
    LOG_INFO("Replay camera focus: ", players[index].player.name,
             " guid=", players[index].player.rawGuid.empty()
                ? std::to_string(players[index].player.guid)
                : players[index].player.rawGuid);
}

bool GodviewReplay::focusPlayerByQuery(const std::string& query) {
    std::string loweredQuery = lowerAscii(query);
    if (loweredQuery.empty() || loweredQuery == "first") {
        focusNextPlayer(1);
        return focusedPlayerGuid_ != 0;
    }

    for (bool allowSubstring : {false, true}) {
        for (const auto& snapshot : recording_.snapshots()) {
            if (snapshot.map != mapId_) continue;
            for (const auto& player : snapshot.players) {
                if (!playerMatchesFocusQuery(player, query, loweredQuery, allowSubstring)) {
                    continue;
                }
                focusedPlayerGuid_ = player.guid;
                LOG_INFO("Replay camera focus: ", player.name,
                         " guid=", player.rawGuid.empty()
                            ? std::to_string(player.guid)
                            : player.rawGuid);
                return true;
            }
        }
    }

    return false;
}

std::optional<GodviewReplay::CameraFocusTarget> GodviewReplay::cameraFocusTarget() const {
    if (!cameraFollowEnabled_) return std::nullopt;

    auto players = recording_.samplePlayers(currentMs_, mapId_);
    if (players.empty()) return std::nullopt;

    const InterpolatedPlayer* selected = nullptr;
    if (focusedPlayerGuid_ != 0) {
        for (const auto& player : players) {
            if (player.player.guid == focusedPlayerGuid_) {
                selected = &player;
                break;
            }
        }
    }
    if (!selected) selected = &players.front();

    CameraFocusTarget target;
    target.guid = selected->player.guid;
    target.name = selected->player.name;
    target.renderPosition = toRenderPosition(selected->player.x,
                                             selected->player.y,
                                             selected->player.z);

    if (selected->player.targetGuid) {
        target.targetGuid = *selected->player.targetGuid;
        for (const auto& player : players) {
            if (player.player.guid != target.targetGuid) continue;
            target.hasTarget = true;
            target.targetName = player.player.name;
            target.targetRenderPosition = toRenderPosition(player.player.x,
                                                           player.player.y,
                                                           player.player.z);
            break;
        }

        if (!target.hasTarget) {
            auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
            for (const auto& creature : creatures) {
                if (creature.creature.guid != target.targetGuid) continue;
                target.hasTarget = true;
                target.targetName = creature.creature.name;
                target.targetRenderPosition = toRenderPosition(creature.creature.x,
                                                               creature.creature.y,
                                                               creature.creature.z);
                break;
            }
        }
    }

    return target;
}

std::string GodviewReplay::focusedPlayerName() const {
    if (focusedPlayerGuid_ == 0) return {};

    auto players = recording_.samplePlayers(currentMs_, mapId_);
    for (const auto& player : players) {
        if (player.player.guid == focusedPlayerGuid_) {
            return player.player.name;
        }
    }

    for (const auto& snapshot : recording_.snapshots()) {
        if (snapshot.map != mapId_) continue;
        auto it = snapshot.playerByGuid.find(focusedPlayerGuid_);
        if (it != snapshot.playerByGuid.end()) {
            return snapshot.players[it->second].name;
        }
    }
    return {};
}

void GodviewReplay::applyGameState(game::GameHandler& gameHandler,
                                   EntitySpawner& entitySpawner,
                                   const std::vector<InterpolatedPlayer>& players,
                                   const std::vector<InterpolatedCreature>& creatures) {
    auto& entities = gameHandler.getEntityManager();
    std::unordered_set<uint64_t> activePlayers;
    activePlayers.reserve(players.size());
    std::unordered_set<uint64_t> activeCreatures;
    activeCreatures.reserve(creatures.size());

    for (const auto& sampled : players) {
        const Player& player = sampled.player;
        activePlayers.insert(player.guid);

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
        entity->setRecordedCombat(player.combat);
        const uint32_t playerDisplayId = player.displayId != 0 ? player.displayId : player.nativeDisplayId;
        const auto displayAppearance = entitySpawner.getHumanoidDisplayAppearance(playerDisplayId);
        entity->setDisplayId(playerDisplayId);
        entity->setMountDisplayId(player.mountDisplayId);
        entity->setPosition(canonical.x, canonical.y, canonical.z, canonicalYaw);

        const uint32_t bytes0 = static_cast<uint32_t>(player.race) |
                                (static_cast<uint32_t>(player.playerClass) << 8) |
                                (static_cast<uint32_t>(player.gender) << 16);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_BYTES_0, bytes0);
        if (playerDisplayId != 0) {
            setFieldIfValid(*entity, game::UF::UNIT_FIELD_DISPLAYID, playerDisplayId);
        }
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_MOUNTDISPLAYID, player.mountDisplayId);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_HEALTH, player.hp);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_MAXHEALTH, std::max<uint32_t>(1, player.maxHp));
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_LEVEL, player.level);
        setTargetFields(*entity, player.targetGuid);

        if (!entitySpawner.isPlayerSpawned(player.guid) && !entitySpawner.isPlayerPending(player.guid)) {
            uint8_t spawnRace = player.race;
            uint8_t spawnGender = player.gender;
            uint32_t appearanceBytes = makeAppearanceBytes(player.guid);
            uint8_t facialFeatures = static_cast<uint8_t>((player.guid >> 32) & 0x03);

            if (displayAppearance) {
                if (isPlayableRace(displayAppearance->raceId)) {
                    spawnRace = displayAppearance->raceId;
                }
                if (displayAppearance->sexId <= 1) {
                    spawnGender = displayAppearance->sexId;
                }
                appearanceBytes = makeAppearanceBytes(displayAppearance->skinId,
                                                      displayAppearance->faceId,
                                                      displayAppearance->hairStyleId,
                                                      displayAppearance->hairColorId);
                facialFeatures = displayAppearance->facialHairId;
            }

            entitySpawner.queuePlayerSpawn(player.guid,
                                           spawnRace,
                                           spawnGender,
                                           appearanceBytes,
                                           facialFeatures,
                                           canonical.x,
                                           canonical.y,
                                           canonical.z,
                                           canonicalYaw);
        }

        const bool useDisplayEquipment = player.equipment.empty() &&
            displayAppearance && hasDisplayEquipment(*displayAppearance);
        size_t currentEquipmentHash = useDisplayEquipment
            ? displayEquipmentHash(*displayAppearance)
            : equipmentHash(player);
        auto equipmentIt = lastPlayerEquipmentHash_.find(player.guid);
        if ((!player.equipment.empty() || useDisplayEquipment || equipmentIt != lastPlayerEquipmentHash_.end()) &&
            (equipmentIt == lastPlayerEquipmentHash_.end() || equipmentIt->second != currentEquipmentHash)) {
            std::array<uint32_t, 19> displayInfoIds{};
            std::array<uint8_t, 19> inventoryTypes{};
            if (useDisplayEquipment) {
                buildDisplayEquipmentArrays(*displayAppearance, displayInfoIds, inventoryTypes);
            } else {
                buildEquipmentArrays(player, displayInfoIds, inventoryTypes);
            }
            entitySpawner.queuePlayerEquipment(player.guid, displayInfoIds, inventoryTypes);
            lastPlayerEquipmentHash_[player.guid] = currentEquipmentHash;
        }
    }

    for (const auto& sampled : creatures) {
        const Creature& creature = sampled.creature;
        const uint32_t creatureDisplayId = creature.displayId != 0 ? creature.displayId : creature.nativeDisplayId;
        if (creatureDisplayId == 0) continue;
        activeCreatures.insert(creature.guid);

        std::shared_ptr<game::Unit> entity;
        auto existing = entities.getEntity(creature.guid);
        if (existing && existing->getType() == game::ObjectType::UNIT) {
            entity = std::static_pointer_cast<game::Unit>(existing);
        } else {
            entity = std::make_shared<game::Unit>(creature.guid);
            entities.addEntity(creature.guid, entity);
        }

        glm::vec3 canonical = coords::serverToCanonical(glm::vec3(creature.x, creature.y, creature.z));
        float canonicalYaw = coords::serverToCanonicalYaw(creature.orientation);
        const uint32_t hp = creature.dead ? 0 : creature.hp;
        entity->setName(creature.name.empty() ? "Creature" : creature.name);
        entity->setEntry(creature.entry);
        entity->setLevel(creature.level);
        entity->setDisplayId(creatureDisplayId);
        entity->setHealth(hp);
        entity->setMaxHealth(std::max<uint32_t>(1, creature.maxHp));
        entity->setRecordedCombat(creature.combat);
        entity->setPosition(canonical.x, canonical.y, canonical.z, canonicalYaw);

        setFieldIfValid(*entity, game::UF::UNIT_FIELD_DISPLAYID, creatureDisplayId);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_HEALTH, hp);
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_MAXHEALTH, std::max<uint32_t>(1, creature.maxHp));
        setFieldIfValid(*entity, game::UF::UNIT_FIELD_LEVEL, creature.level);
        setTargetFields(*entity, creature.targetGuid);

        if (creature.dead) {
            entitySpawner.markCreatureDead(creature.guid);
        } else {
            entitySpawner.unmarkCreatureDead(creature.guid);
        }

        if (!entitySpawner.isCreatureSpawned(creature.guid) && !entitySpawner.isCreaturePending(creature.guid)) {
            entitySpawner.queueCreatureSpawn(creature.guid,
                                             creatureDisplayId,
                                             canonical.x,
                                             canonical.y,
                                             canonical.z,
                                             canonicalYaw);
        }
    }

    despawnMissingPlayers(gameHandler, entitySpawner, activePlayers);
    despawnMissingCreatures(gameHandler, entitySpawner, activeCreatures);
    activePlayerGuids_ = std::move(activePlayers);
    activeCreatureGuids_ = std::move(activeCreatures);
}

void GodviewReplay::despawnMissingPlayers(game::GameHandler& gameHandler,
                                          EntitySpawner& entitySpawner,
                                          const std::unordered_set<uint64_t>& activeGuids) {
    std::vector<uint64_t> toDespawn;
    for (uint64_t guid : activePlayerGuids_) {
        if (!activeGuids.count(guid)) toDespawn.push_back(guid);
    }

    for (uint64_t guid : toDespawn) {
        entitySpawner.despawnPlayer(guid);
        gameHandler.getEntityManager().removeEntity(guid);
        lastMoving_.erase(guid);
        lastPlayerEquipmentHash_.erase(guid);
    }
}

void GodviewReplay::despawnMissingCreatures(game::GameHandler& gameHandler,
                                            EntitySpawner& entitySpawner,
                                            const std::unordered_set<uint64_t>& activeGuids) {
    std::vector<uint64_t> toDespawn;
    for (uint64_t guid : activeCreatureGuids_) {
        if (!activeGuids.count(guid)) toDespawn.push_back(guid);
    }

    for (uint64_t guid : toDespawn) {
        entitySpawner.despawnCreature(guid);
        gameHandler.getEntityManager().removeEntity(guid);
        lastCreatureMoving_.erase(guid);
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
    auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
    applyGameState(gameHandler, entitySpawner, players, creatures);
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

    auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
    for (const auto& sampled : creatures) {
        const Creature& creature = sampled.creature;
        uint32_t instanceId = entitySpawner.getCreatureInstanceId(creature.guid);
        if (instanceId == 0) continue;

        glm::vec3 canonical = coords::serverToCanonical(glm::vec3(creature.x, creature.y, creature.z));
        glm::vec3 renderPos = coords::canonicalToRender(canonical);
        float renderYaw = coords::serverToCanonicalYaw(creature.orientation) + glm::radians(90.0f);
        charRenderer->setInstancePosition(instanceId, renderPos);
        charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, 0.0f, renderYaw));

        if (creature.dead) {
            uint32_t currentAnim = 0;
            float currentTime = 0.0f;
            float currentDuration = 0.0f;
            bool gotAnim = charRenderer->getAnimationState(instanceId, currentAnim, currentTime, currentDuration);
            if (!gotAnim || (currentAnim != rendering::anim::DEATH && currentAnim != rendering::anim::DEAD)) {
                uint32_t deathAnim = charRenderer->hasAnimation(instanceId, rendering::anim::DEATH)
                    ? rendering::anim::DEATH
                    : rendering::anim::DEAD;
                charRenderer->playAnimation(instanceId, deathAnim, false);
            }
            lastCreatureMoving_[creature.guid] = false;
            continue;
        }

        const bool moving = sampled.moving;
        auto lastIt = lastCreatureMoving_.find(creature.guid);
        bool stateChanged = (lastIt == lastCreatureMoving_.end() || lastIt->second != moving);
        if (stateChanged) {
            uint32_t anim = moving ? rendering::anim::RUN
                                   : (creature.combat ? rendering::anim::READY_UNARMED
                                                      : rendering::anim::STAND);
            if (!charRenderer->hasAnimation(instanceId, anim)) {
                anim = moving ? rendering::anim::RUN : rendering::anim::STAND;
            }
            charRenderer->playAnimation(instanceId, anim, true);
            lastCreatureMoving_[creature.guid] = moving;
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
        case SDL_SCANCODE_H:
            overlayVisible_ = !overlayVisible_;
            break;
        case SDL_SCANCODE_F:
            setCameraFollowEnabled(!cameraFollowEnabled_);
            break;
        case SDL_SCANCODE_TAB:
            focusNextPlayer((event.keysym.mod & KMOD_SHIFT) ? -1 : 1);
            break;
        default:
            break;
    }
}

void GodviewReplay::renderOverlay() {
    if (recording_.empty() || !overlayVisible_) return;

    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::SetNextWindowPos(ImVec2(14.0f, 14.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 0.0f), ImVec2(560.0f, 340.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Replay", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const std::string recordingName = std::filesystem::path(recording_.path()).filename().string();
    ImGui::TextUnformatted(recordingName.empty() ? recording_.path().c_str() : recordingName.c_str());
    if (ImGui::Button(paused_ ? "Play" : "Pause")) {
        paused_ = !paused_;
    }
    ImGui::SameLine();
    if (ImGui::Button("|<")) {
        setCurrentMs(static_cast<double>(startMs_));
        paused_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("<<")) {
        setCurrentMs(currentMs_ - 5000.0);
        paused_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(">>")) {
        setCurrentMs(currentMs_ + 5000.0);
        paused_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(">|")) {
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

    ImGui::Separator();
    if (ImGui::Button(cameraFollowEnabled_ ? "Free camera" : "Follow player")) {
        setCameraFollowEnabled(!cameraFollowEnabled_);
    }
    ImGui::SameLine();
    if (ImGui::Button("< Player")) {
        focusNextPlayer(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Player >")) {
        focusNextPlayer(1);
    }
    std::string focusName = focusedPlayerName();
    if (cameraFollowEnabled_) {
        auto focusTarget = cameraFocusTarget();
        if (focusTarget && focusTarget->hasTarget && !focusTarget->targetName.empty()) {
            ImGui::Text("Camera following: %s -> %s",
                        focusName.empty() ? "first active player" : focusName.c_str(),
                        focusTarget->targetName.c_str());
        } else {
            ImGui::Text("Camera following: %s", focusName.empty() ? "first active player" : focusName.c_str());
        }
    } else if (!focusName.empty()) {
        ImGui::Text("Camera focus: %s", focusName.c_str());
    } else {
        ImGui::TextUnformatted("Camera: free observer");
    }

    auto pair = recording_.findSnapshotPair(currentMs_, mapId_);
    const auto& snapshots = recording_.snapshots();
    size_t players = pair.valid ? snapshots[pair.prev].players.size() : 0;
    size_t creatures = pair.valid ? snapshots[pair.prev].creatures.size() : 0;
    ImGui::Text("Map %u  snapshots %zu  players %zu  creatures %zu",
                mapId_,
                recording_.snapshotCountForMap(mapId_),
                players,
                creatures);
    ImGui::Text("%.1fs / %.1fs",
                static_cast<float>((currentMs_ - static_cast<double>(startMs_)) / 1000.0),
                durationSeconds);

    ImGui::End();
}

} // namespace core
} // namespace wowee
