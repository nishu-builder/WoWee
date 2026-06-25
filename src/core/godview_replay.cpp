#include "core/godview_replay.hpp"

#include "core/coordinates.hpp"
#include "core/entity_spawner.hpp"
#include "core/logger.hpp"
#include "game/entity.hpp"
#include "game/game_handler.hpp"
#include "game/inventory.hpp"
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
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace wowee {
namespace core {

namespace {

constexpr uint64_t kReplayMountGuidStart = 0xF000000000000000ull;
constexpr float kReplayMountFallbackRiderZOffset = 1.35f;
constexpr float kReplayMountSeatHeightFraction = 0.58f;
constexpr float kReplayMountSeatMinZOffset = 0.8f;
constexpr float kReplayMountSeatMaxZOffset = 3.25f;
constexpr float kReplayInferredAttackMaxRange = 45.0f;
constexpr float kReplayInferredAttackStartAlpha = 0.02f;
constexpr float kReplayInferredAttackEndAlpha = 0.85f;
constexpr double kReplayDamageTextWindowMs = 750.0;
constexpr double kReplayEventTargetContextWindowMs = 2500.0;
constexpr double kReplayEventFutureSlackMs = 0.5;

struct ReplayAttackPulse {
    uint64_t sourceGuid = 0;
    uint64_t targetGuid = 0;
    uint64_t snapshotMs = 0;
    bool sourceCreature = false;
};

struct ReplayDamagedEntity {
    uint64_t guid = 0;
    glm::vec3 position{0.0f};
};

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

bool shouldRenderPlayerAsDisplayModel(uint32_t displayId,
                                      uint32_t nativeDisplayId,
                                      const std::optional<EntitySpawner::HumanoidDisplayAppearance>& appearance) {
    if (displayId == 0) return false;
    if (nativeDisplayId != 0 && displayId == nativeDisplayId) return false;
    if (!appearance) return true;
    return !isPlayableRace(appearance->raceId);
}

uint32_t pickFirstAvailableAnimation(rendering::CharacterRenderer& characterRenderer,
                                     uint32_t instanceId,
                                     std::initializer_list<uint32_t> candidates) {
    for (uint32_t candidate : candidates) {
        if (candidate != 0 && characterRenderer.hasAnimation(instanceId, candidate)) {
            return candidate;
        }
    }
    return 0;
}

const GodviewRecording::Equipment* findReplayEquipmentSlot(const GodviewRecording::Player& player,
                                                           uint8_t slot) {
    for (const auto& equipment : player.equipment) {
        if (equipment.slot == slot &&
            equipment.itemClass == 2 &&
            (equipment.displayId != 0 || equipment.itemId != 0)) {
            return &equipment;
        }
    }
    return nullptr;
}

uint64_t replayAttackPulseKey(const ReplayAttackPulse& pulse) {
    uint64_t hash = pulse.snapshotMs ^ (pulse.targetGuid + 0x9e3779b97f4a7c15ull);
    hash ^= pulse.sourceCreature ? 0x8000000000000000ull : 0ull;
    return hash != 0 ? hash : 1;
}

float replayDistanceSq(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

template <typename EntityT>
glm::vec3 replayEntityPosition(const EntityT& entity) {
    return glm::vec3(entity.x, entity.y, entity.z);
}

void addReplayDamagePulsesForTarget(const GodviewRecording::Snapshot& next,
                                    const ReplayDamagedEntity& damaged,
                                    std::unordered_map<uint64_t, ReplayAttackPulse>& pulses) {
    auto addPulse = [&](uint64_t sourceGuid, bool sourceCreature) {
        if (sourceGuid == 0 || sourceGuid == damaged.guid || pulses.count(sourceGuid) > 0) return;
        pulses[sourceGuid] = ReplayAttackPulse{
            sourceGuid,
            damaged.guid,
            next.ms,
            sourceCreature,
        };
    };

    for (const auto& player : next.players) {
        if (player.combat && player.targetGuid && *player.targetGuid == damaged.guid) {
            addPulse(player.guid, false);
        }
    }
    for (const auto& creature : next.creatures) {
        if (!creature.dead && creature.combat && creature.targetGuid && *creature.targetGuid == damaged.guid) {
            addPulse(creature.guid, true);
        }
    }
}

bool hasReplayAttackPulseForTarget(const std::unordered_map<uint64_t, ReplayAttackPulse>& pulses,
                                   uint64_t targetGuid) {
    return std::any_of(pulses.begin(), pulses.end(), [targetGuid](const auto& entry) {
        return entry.second.targetGuid == targetGuid;
    });
}

bool isReplayDamageEvent(const GodviewRecording::ReplayEvent& event) {
    return event.kind == "damage" && event.amount > 0 && event.sourceGuid != 0 && event.targetGuid != 0;
}

uint64_t replayDamageTextKey(uint64_t snapshotMs, const GodviewRecording::ReplayEvent& event) {
    uint64_t hash = snapshotMs ^ 0x9e3779b97f4a7c15ull;
    auto mix = [&hash](uint64_t value) {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    };
    mix(event.sourceGuid);
    mix(event.targetGuid);
    mix(event.amount);
    return hash != 0 ? hash : 1;
}

template <typename Callback>
bool forEachNearbyReplayEventSnapshot(const GodviewRecording& recording,
                                      const GodviewRecording::SnapshotPair& pair,
                                      double currentMs,
                                      double windowMs,
                                      Callback callback) {
    if (!pair.valid) return false;

    const std::array<size_t, 2> indices{pair.prev, pair.next};
    for (size_t slot = 0; slot < indices.size(); ++slot) {
        if (slot > 0 && indices[slot] == indices[0]) continue;

        const auto& snapshot = recording.snapshots()[indices[slot]];
        const double snapshotMs = static_cast<double>(snapshot.ms);
        if (snapshotMs > currentMs + kReplayEventFutureSlackMs) {
            continue;
        }
        if (currentMs - snapshotMs > windowMs) continue;
        if (callback(snapshot)) return true;
    }
    return false;
}

void addExplicitReplayDamagePulses(const GodviewRecording::Snapshot& next,
                                   std::unordered_map<uint64_t, ReplayAttackPulse>& pulses) {
    for (const auto& event : next.events) {
        if (!isReplayDamageEvent(event) || pulses.count(event.sourceGuid) > 0) continue;

        const bool sourceCreature = next.creatureByGuid.count(event.sourceGuid) > 0;
        if (!sourceCreature && next.playerByGuid.count(event.sourceGuid) == 0) continue;

        pulses[event.sourceGuid] = ReplayAttackPulse{
            event.sourceGuid,
            event.targetGuid,
            next.ms,
            sourceCreature,
        };
    }
}

void addNearestReplayDamagePulse(const GodviewRecording::Snapshot& next,
                                 const ReplayDamagedEntity& damaged,
                                 std::unordered_map<uint64_t, ReplayAttackPulse>& pulses) {
    if (hasReplayAttackPulseForTarget(pulses, damaged.guid)) return;

    const float maxDistSq = kReplayInferredAttackMaxRange * kReplayInferredAttackMaxRange;
    uint64_t bestGuid = 0;
    bool bestCreature = false;
    float bestDistSq = maxDistSq;

    auto consider = [&](uint64_t guid, bool sourceCreature, const glm::vec3& position) {
        if (guid == 0 || guid == damaged.guid || pulses.count(guid) > 0) return;
        const float distSq = replayDistanceSq(position, damaged.position);
        if (distSq <= bestDistSq) {
            bestGuid = guid;
            bestCreature = sourceCreature;
            bestDistSq = distSq;
        }
    };

    for (const auto& player : next.players) {
        if (player.combat && player.hp > 0) {
            consider(player.guid, false, replayEntityPosition(player));
        }
    }
    for (const auto& creature : next.creatures) {
        if (!creature.dead && creature.combat && creature.hp > 0) {
            consider(creature.guid, true, replayEntityPosition(creature));
        }
    }

    if (bestGuid != 0) {
        pulses[bestGuid] = ReplayAttackPulse{
            bestGuid,
            damaged.guid,
            next.ms,
            bestCreature,
        };
    }
}

std::unordered_map<uint64_t, ReplayAttackPulse> inferReplayAttackPulses(
    const GodviewRecording& recording,
    double currentMs,
    uint32_t mapId) {
    std::unordered_map<uint64_t, ReplayAttackPulse> pulses;
    GodviewRecording::SnapshotPair pair = recording.findSnapshotPair(currentMs, mapId);
    if (!pair.valid ||
        pair.prev == pair.next ||
        pair.alpha < kReplayInferredAttackStartAlpha ||
        pair.alpha > kReplayInferredAttackEndAlpha) {
        return pulses;
    }

    const auto& snapshots = recording.snapshots();
    const auto& prev = snapshots[pair.prev];
    const auto& next = snapshots[pair.next];
    std::vector<ReplayDamagedEntity> damagedEntities;

    addExplicitReplayDamagePulses(next, pulses);

    for (const auto& nextPlayer : next.players) {
        auto prevIt = prev.playerByGuid.find(nextPlayer.guid);
        if (prevIt == prev.playerByGuid.end()) continue;
        const auto& prevPlayer = prev.players[prevIt->second];
        if (nextPlayer.hp < prevPlayer.hp) {
            damagedEntities.push_back({nextPlayer.guid, replayEntityPosition(nextPlayer)});
        }
    }
    for (const auto& nextCreature : next.creatures) {
        auto prevIt = prev.creatureByGuid.find(nextCreature.guid);
        if (prevIt == prev.creatureByGuid.end()) continue;
        const auto& prevCreature = prev.creatures[prevIt->second];
        if (nextCreature.hp < prevCreature.hp) {
            damagedEntities.push_back({nextCreature.guid, replayEntityPosition(nextCreature)});
        }
    }

    for (const auto& damaged : damagedEntities) {
        addReplayDamagePulsesForTarget(next, damaged, pulses);
    }
    for (const auto& damaged : damagedEntities) {
        addNearestReplayDamagePulse(next, damaged, pulses);
    }
    return pulses;
}

uint32_t replayReadyAnimationForWeapon(const GodviewRecording::Equipment& equipment,
                                       rendering::CharacterRenderer& characterRenderer,
                                       uint32_t instanceId) {
    using namespace rendering;

    switch (equipment.subclass) {
        case 2:  // Bow
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED});
        case 3:  // Gun
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_RIFLE, anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED});
        case 16: // Thrown
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_THROWN, anim::READY_1H, anim::READY_UNARMED});
        case 17: // Crossbow in some client tables
        case 18: // Crossbow in classic item tables
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_CROSSBOW, anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED});
        case 6:  // Polearm
        case 10: // Staff
        case 20: // Fishing pole
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_2H_LOOSE, anim::READY_2H, anim::READY_1H, anim::READY_UNARMED});
        case 13: // Fist weapon
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_FIST_1H, anim::READY_FIST, anim::READY_1H, anim::READY_UNARMED});
        default:
            break;
    }

    switch (equipment.inventoryType) {
        case game::InvType::RANGED_BOW:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_BOW, anim::READY_1H, anim::READY_UNARMED});
        case game::InvType::RANGED_GUN:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_RIFLE, anim::READY_CROSSBOW, anim::READY_BOW,
                                                anim::READY_1H, anim::READY_UNARMED});
        case game::InvType::THROWN:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_THROWN, anim::READY_1H, anim::READY_UNARMED});
        case game::InvType::TWO_HAND:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_2H, anim::READY_2H_LOOSE, anim::READY_1H, anim::READY_UNARMED});
        default:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::READY_1H, anim::READY_UNARMED});
    }
}

uint32_t replayAttackAnimationForWeapon(const GodviewRecording::Equipment& equipment,
                                        rendering::CharacterRenderer& characterRenderer,
                                        uint32_t instanceId) {
    using namespace rendering;

    switch (equipment.subclass) {
        case 2:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_BOW, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        case 3:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_RIFLE, anim::ATTACK_BOW, anim::ATTACK_1H,
                                                anim::ATTACK_UNARMED});
        case 16:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_THROWN, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        case 17:
        case 18:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_BOW, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        case 6:
        case 10:
        case 20:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_2H_LOOSE, anim::ATTACK_2H, anim::ATTACK_1H,
                                                anim::ATTACK_UNARMED});
        case 13:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_FIST_1H, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        case 15:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_1H_PIERCE, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        default:
            break;
    }

    switch (equipment.inventoryType) {
        case game::InvType::RANGED_BOW:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_BOW, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        case game::InvType::RANGED_GUN:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_RIFLE, anim::ATTACK_BOW, anim::ATTACK_1H,
                                                anim::ATTACK_UNARMED});
        case game::InvType::THROWN:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_THROWN, anim::ATTACK_1H, anim::ATTACK_UNARMED});
        case game::InvType::TWO_HAND:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_2H, anim::ATTACK_2H_LOOSE, anim::ATTACK_1H,
                                                anim::ATTACK_UNARMED});
        default:
            return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                               {anim::ATTACK_1H, anim::ATTACK_UNARMED});
    }
}

uint32_t replayPlayerAttackAnimation(const GodviewRecording::Player& player,
                                     rendering::CharacterRenderer& characterRenderer,
                                     uint32_t instanceId) {
    if (const auto* mainHand = findReplayEquipmentSlot(player, 15)) {
        if (uint32_t anim = replayAttackAnimationForWeapon(*mainHand, characterRenderer, instanceId)) {
            return anim;
        }
    }
    if (const auto* offHand = findReplayEquipmentSlot(player, 16)) {
        if (uint32_t anim = replayAttackAnimationForWeapon(*offHand, characterRenderer, instanceId)) {
            return anim;
        }
    }
    if (const auto* ranged = findReplayEquipmentSlot(player, 17)) {
        if (uint32_t anim = replayAttackAnimationForWeapon(*ranged, characterRenderer, instanceId)) {
            return anim;
        }
    }
    return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                       {rendering::anim::ATTACK_UNARMED,
                                        rendering::anim::ATTACK_1H,
                                        rendering::anim::STAND});
}

uint32_t replayCreatureAttackAnimation(rendering::CharacterRenderer& characterRenderer,
                                       uint32_t instanceId) {
    return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                       {rendering::anim::ATTACK_UNARMED,
                                        rendering::anim::ATTACK_1H,
                                        rendering::anim::ATTACK_2H,
                                        rendering::anim::ATTACK_2H_LOOSE,
                                        rendering::anim::STAND});
}

uint32_t replayPlayerCombatReadyAnimation(const GodviewRecording::Player& player,
                                          rendering::CharacterRenderer& characterRenderer,
                                          uint32_t instanceId) {
    if (const auto* mainHand = findReplayEquipmentSlot(player, 15)) {
        if (uint32_t anim = replayReadyAnimationForWeapon(*mainHand, characterRenderer, instanceId)) {
            return anim;
        }
    }

    if (const auto* offHand = findReplayEquipmentSlot(player, 16)) {
        if (uint32_t anim = replayReadyAnimationForWeapon(*offHand, characterRenderer, instanceId)) {
            return anim;
        }
    }

    if (const auto* ranged = findReplayEquipmentSlot(player, 17)) {
        if (uint32_t anim = replayReadyAnimationForWeapon(*ranged, characterRenderer, instanceId)) {
            return anim;
        }
    }

    return pickFirstAvailableAnimation(characterRenderer, instanceId,
                                       {rendering::anim::READY_UNARMED,
                                        rendering::anim::READY_1H,
                                        rendering::anim::STAND});
}

uint32_t replayPlayerAnimation(const GodviewRecording::Player& player,
                               bool moving,
                               bool mounted,
                               rendering::CharacterRenderer& characterRenderer,
                               uint32_t instanceId) {
    if (mounted && characterRenderer.hasAnimation(instanceId, rendering::anim::MOUNT)) {
        return rendering::anim::MOUNT;
    }

    if (moving) {
        if (characterRenderer.hasAnimation(instanceId, rendering::anim::RUN)) {
            return rendering::anim::RUN;
        }
        return rendering::anim::STAND;
    }

    if (player.combat) {
        return replayPlayerCombatReadyAnimation(player, characterRenderer, instanceId);
    }

    return rendering::anim::STAND;
}

uint32_t replayCreatureAnimation(const GodviewRecording::Creature& creature,
                                 bool moving,
                                 rendering::CharacterRenderer& characterRenderer,
                                 uint32_t instanceId) {
    if (moving) {
        if (characterRenderer.hasAnimation(instanceId, rendering::anim::RUN)) {
            return rendering::anim::RUN;
        }
        return rendering::anim::STAND;
    }

    if (!creature.combat) {
        return rendering::anim::STAND;
    }

    if (uint32_t anim = pickFirstAvailableAnimation(characterRenderer, instanceId,
                                                   {rendering::anim::READY_UNARMED,
                                                    rendering::anim::READY_1H,
                                                    rendering::anim::COMBAT_WOUND,
                                                    rendering::anim::STAND_WOUND,
                                                    rendering::anim::STAND})) {
        return anim;
    }
    return rendering::anim::STAND;
}

float replayMountRiderZOffset(rendering::CharacterRenderer& characterRenderer,
                              uint32_t mountInstanceId) {
    glm::vec3 mountPosition{};
    glm::vec3 boundsCenter{};
    float boundsRadius = 0.0f;
    float footZ = 0.0f;
    if (!characterRenderer.getInstancePosition(mountInstanceId, mountPosition) ||
        !characterRenderer.getInstanceBounds(mountInstanceId, boundsCenter, boundsRadius) ||
        !characterRenderer.getInstanceFootZ(mountInstanceId, footZ)) {
        return kReplayMountFallbackRiderZOffset;
    }

    (void)boundsRadius;
    const float mountHeight = std::max(0.0f, (boundsCenter.z - footZ) * 2.0f);
    if (mountHeight <= 0.25f) {
        return kReplayMountFallbackRiderZOffset;
    }

    const float seatZ = footZ + mountHeight * kReplayMountSeatHeightFraction;
    const float offset = seatZ - mountPosition.z;
    if (!std::isfinite(offset)) {
        return kReplayMountFallbackRiderZOffset;
    }

    return std::clamp(offset, kReplayMountSeatMinZOffset, kReplayMountSeatMaxZOffset);
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
    lastPlayerCombat_.clear();
    lastPlayerMounted_.clear();
    lastCreatureMoving_.clear();
    lastCreatureCombat_.clear();
    lastCreatureDead_.clear();
    lastMountMoving_.clear();
    lastReplayAttackPulseKey_.clear();
    emittedReplayDamageTextKeys_.clear();
    activeReplayAttackGuids_.clear();
    lastPlayerEquipmentHash_.clear();
    displayOverridePlayerGuids_.clear();
    displayOverridePlayerDisplayIds_.clear();
    replayMountGuids_.clear();
    replayMountDisplayIds_.clear();
    replayMountSpawnQueued_.clear();
    nextReplayMountGuid_ = kReplayMountGuidStart;

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
    focusedEventTargetGuid_ = 0;
    focusedEventTargetMs_ = 0.0;

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
    lastPlayerCombat_.clear();
    lastPlayerMounted_.clear();
    lastCreatureMoving_.clear();
    lastCreatureCombat_.clear();
    lastCreatureDead_.clear();
    lastMountMoving_.clear();
    lastReplayAttackPulseKey_.clear();
    emittedReplayDamageTextKeys_.clear();
    activeReplayAttackGuids_.clear();
    lastPlayerEquipmentHash_.clear();
    displayOverridePlayerGuids_.clear();
    displayOverridePlayerDisplayIds_.clear();
    replayMountGuids_.clear();
    replayMountDisplayIds_.clear();
    replayMountSpawnQueued_.clear();
    nextReplayMountGuid_ = kReplayMountGuidStart;
    focusedEventTargetGuid_ = 0;
    focusedEventTargetMs_ = 0.0;
}

void GodviewReplay::setCurrentMs(double value) {
    if (recording_.empty()) {
        currentMs_ = 0.0;
        return;
    }
    const double nextMs = std::clamp(value, static_cast<double>(startMs_), static_cast<double>(endMs_));
    if (nextMs + 1.0 < currentMs_) {
        emittedReplayDamageTextKeys_.clear();
    }
    currentMs_ = nextMs;
}

void GodviewReplay::emitReplayDamageText(game::GameHandler& gameHandler) {
    GodviewRecording::SnapshotPair pair = recording_.findSnapshotPair(currentMs_, mapId_);
    forEachNearbyReplayEventSnapshot(recording_,
                                     pair,
                                     currentMs_,
                                     kReplayDamageTextWindowMs,
                                     [&](const GodviewRecording::Snapshot& snapshot) {
        for (const auto& event : snapshot.events) {
            if (!isReplayDamageEvent(event)) continue;
            const uint64_t key = replayDamageTextKey(snapshot.ms, event);
            if (emittedReplayDamageTextKeys_.count(key) > 0) continue;

            const bool sourceIsPlayer = snapshot.playerByGuid.count(event.sourceGuid) > 0;
            gameHandler.addCombatText(game::CombatTextEntry::SPELL_DAMAGE,
                                      static_cast<int32_t>(event.amount),
                                      event.spellId.value_or(0),
                                      sourceIsPlayer,
                                      0,
                                      event.sourceGuid,
                                      event.targetGuid);
            emittedReplayDamageTextKeys_.insert(key);
        }
        return false;
    });
}

std::optional<uint64_t> GodviewReplay::activeFocusedEventTargetGuid(
    const std::vector<InterpolatedPlayer>& players,
    const std::vector<InterpolatedCreature>& creatures) const {
    if (focusedPlayerGuid_ == 0 || focusedEventTargetGuid_ == 0 || focusedEventTargetMs_ <= 0.0) {
        return std::nullopt;
    }
    if (currentMs_ + kReplayEventFutureSlackMs < focusedEventTargetMs_) {
        return std::nullopt;
    }
    if (currentMs_ - focusedEventTargetMs_ > kReplayEventTargetContextWindowMs) {
        return std::nullopt;
    }

    bool focusedPlayerPresent = false;
    bool targetPresent = false;
    for (const auto& sampled : players) {
        if (sampled.player.guid == focusedPlayerGuid_) {
            focusedPlayerPresent = true;
        }
        if (sampled.player.guid == focusedEventTargetGuid_) {
            targetPresent = true;
        }
    }
    for (const auto& sampled : creatures) {
        if (sampled.creature.guid == focusedEventTargetGuid_) {
            targetPresent = true;
            break;
        }
    }

    if (!focusedPlayerPresent || !targetPresent) {
        return std::nullopt;
    }
    return focusedEventTargetGuid_;
}

uint64_t GodviewReplay::replayMountGuidForPlayer(uint64_t playerGuid) {
    auto it = replayMountGuids_.find(playerGuid);
    if (it != replayMountGuids_.end()) return it->second;

    uint64_t mountGuid = nextReplayMountGuid_++;
    replayMountGuids_[playerGuid] = mountGuid;
    return mountGuid;
}

void GodviewReplay::despawnReplayMount(EntitySpawner& entitySpawner, uint64_t playerGuid) {
    auto guidIt = replayMountGuids_.find(playerGuid);
    if (guidIt != replayMountGuids_.end()) {
        entitySpawner.despawnCreature(guidIt->second);
        lastMountMoving_.erase(guidIt->second);
    }
    replayMountDisplayIds_.erase(playerGuid);
    replayMountSpawnQueued_.erase(playerGuid);
    lastPlayerMounted_.erase(playerGuid);
}

void GodviewReplay::syncReplayMountForPlayer(EntitySpawner& entitySpawner,
                                             const Player& player,
                                             const glm::vec3& canonicalPosition,
                                             float canonicalYaw) {
    if (player.mountDisplayId == 0) {
        despawnReplayMount(entitySpawner, player.guid);
        return;
    }

    const uint64_t mountGuid = replayMountGuidForPlayer(player.guid);
    auto displayIt = replayMountDisplayIds_.find(player.guid);
    const bool firstMountSample = displayIt == replayMountDisplayIds_.end();
    const bool displayChanged = !firstMountSample && displayIt->second != player.mountDisplayId;
    if (displayChanged) {
        entitySpawner.despawnCreature(mountGuid);
        lastMountMoving_.erase(mountGuid);
        replayMountSpawnQueued_.erase(player.guid);
    }

    replayMountDisplayIds_[player.guid] = player.mountDisplayId;
    if (firstMountSample || displayChanged) {
        LOG_INFO("Replay player mount: ", player.name.empty() ? "Player" : player.name,
                 " mountDisplayId=", player.mountDisplayId,
                 " playerGuid=", player.rawGuid.empty() ? std::to_string(player.guid) : player.rawGuid,
                 " mountGuid=0x", std::hex, mountGuid, std::dec);
    }

    const bool spawnQueued = replayMountSpawnQueued_.count(player.guid) > 0;
    if (!spawnQueued &&
        !entitySpawner.isCreatureSpawned(mountGuid) &&
        !entitySpawner.isCreaturePending(mountGuid)) {
        entitySpawner.queueCreatureSpawn(mountGuid,
                                         player.mountDisplayId,
                                         canonicalPosition.x,
                                         canonicalPosition.y,
                                         canonicalPosition.z,
                                         canonicalYaw);
        replayMountSpawnQueued_.insert(player.guid);
    }
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
        focusedEventTargetGuid_ = 0;
        focusedEventTargetMs_ = 0.0;
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
    focusedEventTargetGuid_ = 0;
    focusedEventTargetMs_ = 0.0;
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
    if (loweredQuery == "event" || loweredQuery == "target/combat") {
        return focusEventPlayer(GodviewRecording::EventKind::TargetOrCombat);
    }
    if (loweredQuery == "target") {
        return focusEventPlayer(GodviewRecording::EventKind::Target);
    }
    if (loweredQuery == "combat") {
        return focusEventPlayer(GodviewRecording::EventKind::Combat);
    }
    if (loweredQuery == "damage") {
        return focusEventPlayer(GodviewRecording::EventKind::Damage);
    }
    if (loweredQuery == "death" || loweredQuery == "dead") {
        return focusEventPlayer(GodviewRecording::EventKind::Death);
    }

    for (bool allowSubstring : {false, true}) {
        for (const auto& snapshot : recording_.snapshots()) {
            if (snapshot.map != mapId_) continue;
            for (const auto& player : snapshot.players) {
                if (!playerMatchesFocusQuery(player, query, loweredQuery, allowSubstring)) {
                    continue;
                }
                focusedPlayerGuid_ = player.guid;
                focusedEventTargetGuid_ = 0;
                focusedEventTargetMs_ = 0.0;
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

bool GodviewReplay::focusEventPlayer(GodviewRecording::EventKind kind) {
    auto players = recording_.samplePlayers(currentMs_, mapId_);
    focusedEventTargetGuid_ = 0;
    focusedEventTargetMs_ = 0.0;
    if (players.empty()) return false;

    auto setFocusedPlayer = [this](const Player& player,
                                   const char* reason,
                                   uint64_t eventTargetGuid = 0) {
        focusedPlayerGuid_ = player.guid;
        focusedEventTargetGuid_ = eventTargetGuid;
        focusedEventTargetMs_ = eventTargetGuid != 0 ? currentMs_ : 0.0;
        LOG_INFO("Replay camera focus: ", player.name,
                 " guid=", player.rawGuid.empty()
                    ? std::to_string(player.guid)
                    : player.rawGuid,
                 " (", reason, ")");
        return true;
    };

    if (kind == GodviewRecording::EventKind::Death) {
        for (const auto& sampled : players) {
            if (sampled.player.hp == 0) {
                return setFocusedPlayer(sampled.player, "event death");
            }
        }

        auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
        for (const auto& creature : creatures) {
            if (!creature.creature.dead && creature.creature.hp != 0) continue;

            const Player* nearestPlayer = nullptr;
            float nearestDistSq = std::numeric_limits<float>::max();
            for (const auto& sampled : players) {
                if (creature.creature.targetGuid &&
                    sampled.player.guid == *creature.creature.targetGuid) {
                    return setFocusedPlayer(sampled.player,
                                            "event death target",
                                            creature.creature.guid);
                }

                const float dx = sampled.player.x - creature.creature.x;
                const float dy = sampled.player.y - creature.creature.y;
                const float dz = sampled.player.z - creature.creature.z;
                const float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq < nearestDistSq) {
                    nearestDistSq = distSq;
                    nearestPlayer = &sampled.player;
                }
            }

            if (nearestPlayer) {
                return setFocusedPlayer(*nearestPlayer,
                                        "event death nearest",
                                        creature.creature.guid);
            }
        }

        return false;
    }

    if (kind == GodviewRecording::EventKind::Damage) {
        GodviewRecording::SnapshotPair pair = recording_.findSnapshotPair(currentMs_, mapId_);
        auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
        bool focused = forEachNearbyReplayEventSnapshot(
            recording_,
            pair,
            currentMs_,
            kReplayDamageTextWindowMs,
            [&](const GodviewRecording::Snapshot& snapshot) {
                for (const auto& event : snapshot.events) {
                    if (!isReplayDamageEvent(event)) continue;
                    for (const auto& sampled : players) {
                        if (sampled.player.guid == event.sourceGuid) {
                            return setFocusedPlayer(sampled.player, "event damage source", event.targetGuid);
                        }
                        if (sampled.player.guid == event.targetGuid) {
                            return setFocusedPlayer(sampled.player, "event damage target", event.sourceGuid);
                        }
                    }

                    const Creature* damagedCreature = nullptr;
                    for (const auto& sampled : creatures) {
                        if (sampled.creature.guid == event.targetGuid) {
                            damagedCreature = &sampled.creature;
                            break;
                        }
                    }
                    if (!damagedCreature) continue;

                    const Player* nearestPlayer = nullptr;
                    float nearestDistSq = std::numeric_limits<float>::max();
                    for (const auto& sampled : players) {
                        const float dx = sampled.player.x - damagedCreature->x;
                        const float dy = sampled.player.y - damagedCreature->y;
                        const float dz = sampled.player.z - damagedCreature->z;
                        const float distSq = dx * dx + dy * dy + dz * dz;
                        if (distSq < nearestDistSq) {
                            nearestDistSq = distSq;
                            nearestPlayer = &sampled.player;
                        }
                    }
                    if (nearestPlayer) {
                        return setFocusedPlayer(*nearestPlayer, "event damage nearest", event.targetGuid);
                    }
                }
                return false;
            });
        if (focused) {
            return true;
        }
    }

    if (kind != GodviewRecording::EventKind::Target) {
        for (const auto& sampled : players) {
            if (sampled.player.combat) {
                return setFocusedPlayer(sampled.player, "event combat");
            }
        }
    }

    if (kind != GodviewRecording::EventKind::Combat) {
        for (const auto& sampled : players) {
            if (sampled.player.targetGuid) {
                return setFocusedPlayer(sampled.player, "event target");
            }
        }
    }

    auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
    for (const auto& creature : creatures) {
        if (!creature.creature.targetGuid) continue;
        if (kind == GodviewRecording::EventKind::Combat && !creature.creature.combat) continue;
        const uint64_t targetGuid = *creature.creature.targetGuid;
        for (const auto& sampled : players) {
            if (sampled.player.guid == targetGuid) {
                return setFocusedPlayer(sampled.player, "event creature target");
            }
        }
    }

    return false;
}

void GodviewReplay::seekToMs(double value, bool pause) {
    setCurrentMs(value);
    paused_ = pause;
}

bool GodviewReplay::seekEvent(GodviewRecording::EventKind kind,
                              int direction,
                              bool includeCurrent) {
    if (recording_.empty()) return false;

    direction = direction < 0 ? -1 : 1;
    double queryMs = currentMs_;
    if (includeCurrent) {
        queryMs += direction > 0 ? -1.0 : 1.0;
    }

    auto eventMs = recording_.findEventMs(queryMs, mapId_, kind, direction);
    if (!eventMs) return false;

    setCurrentMs(static_cast<double>(*eventMs));
    paused_ = true;
    const char* eventName = "target/combat";
    if (kind == GodviewRecording::EventKind::Combat) {
        eventName = "combat";
    } else if (kind == GodviewRecording::EventKind::Target) {
        eventName = "target";
    } else if (kind == GodviewRecording::EventKind::Damage) {
        eventName = "damage";
    } else if (kind == GodviewRecording::EventKind::Death) {
        eventName = "death";
    }
    LOG_INFO("Replay ", eventName, " event: +", currentMs_ - static_cast<double>(startMs_), "ms");
    return true;
}

bool GodviewReplay::seekTargetOrCombatEvent(int direction, bool includeCurrent) {
    return seekEvent(GodviewRecording::EventKind::TargetOrCombat, direction, includeCurrent);
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

    auto creatures = recording_.sampleCreatures(currentMs_, mapId_);
    const auto eventTargetGuid = activeFocusedEventTargetGuid(players, creatures);
    const uint64_t cameraTargetGuid = eventTargetGuid
        ? *eventTargetGuid
        : selected->player.targetGuid.value_or(0);

    if (cameraTargetGuid != 0) {
        target.targetGuid = cameraTargetGuid;
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

    const auto activeEventTargetGuid = activeFocusedEventTargetGuid(players, creatures);

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
        const bool useDisplayModel = shouldRenderPlayerAsDisplayModel(playerDisplayId,
                                                                      player.nativeDisplayId,
                                                                      displayAppearance);
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
        const auto targetGuid = player.guid == focusedPlayerGuid_ && activeEventTargetGuid
            ? activeEventTargetGuid
            : player.targetGuid;
        setTargetFields(*entity, targetGuid);
        syncReplayMountForPlayer(entitySpawner, player, canonical, canonicalYaw);

        if (useDisplayModel) {
            if (entitySpawner.isPlayerSpawned(player.guid)) {
                entitySpawner.despawnPlayer(player.guid);
            }
            bool needsDisplayRespawn = false;
            auto displayIt = displayOverridePlayerDisplayIds_.find(player.guid);
            if (displayIt != displayOverridePlayerDisplayIds_.end() && displayIt->second != playerDisplayId) {
                entitySpawner.despawnCreature(player.guid);
                lastMoving_.erase(player.guid);
                lastPlayerCombat_.erase(player.guid);
                lastPlayerMounted_.erase(player.guid);
                needsDisplayRespawn = true;
            }
            displayOverridePlayerDisplayIds_[player.guid] = playerDisplayId;

            auto insertResult = displayOverridePlayerGuids_.insert(player.guid);
            if (insertResult.second) {
                LOG_INFO("Replay player display override: ", player.name,
                         " displayId=", playerDisplayId,
                         " guid=", player.rawGuid.empty() ? std::to_string(player.guid) : player.rawGuid);
            }
            if (needsDisplayRespawn ||
                (!entitySpawner.isCreatureSpawned(player.guid) && !entitySpawner.isCreaturePending(player.guid))) {
                entitySpawner.queueCreatureSpawn(player.guid,
                                                 playerDisplayId,
                                                 canonical.x,
                                                 canonical.y,
                                                 canonical.z,
                                                 canonicalYaw);
            }
            lastPlayerEquipmentHash_.erase(player.guid);
            continue;
        }

        if (displayOverridePlayerGuids_.erase(player.guid) > 0) {
            entitySpawner.despawnCreature(player.guid);
            displayOverridePlayerDisplayIds_.erase(player.guid);
            lastMoving_.erase(player.guid);
            lastPlayerCombat_.erase(player.guid);
            lastPlayerMounted_.erase(player.guid);
        }

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
        if (displayOverridePlayerGuids_.erase(guid) > 0) {
            entitySpawner.despawnCreature(guid);
            displayOverridePlayerDisplayIds_.erase(guid);
        }
        gameHandler.getEntityManager().removeEntity(guid);
        lastMoving_.erase(guid);
        lastPlayerCombat_.erase(guid);
        lastPlayerMounted_.erase(guid);
        lastReplayAttackPulseKey_.erase(guid);
        activeReplayAttackGuids_.erase(guid);
        lastPlayerEquipmentHash_.erase(guid);
        despawnReplayMount(entitySpawner, guid);
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
        lastCreatureCombat_.erase(guid);
        lastCreatureDead_.erase(guid);
        lastReplayAttackPulseKey_.erase(guid);
        activeReplayAttackGuids_.erase(guid);
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
    auto* charRenderer = renderer.getCharacterRenderer();
    if (!charRenderer) return;

    const auto attackPulses = inferReplayAttackPulses(recording_, currentMs_, mapId_);
    emitReplayDamageText(gameHandler);
    std::unordered_set<uint64_t> currentlyAttacking;

    auto players = recording_.samplePlayers(currentMs_, mapId_);
    for (const auto& sampled : players) {
        const Player& player = sampled.player;
        const bool displayOverride = displayOverridePlayerGuids_.count(player.guid) > 0;
        const bool mounted = player.mountDisplayId != 0 &&
            replayMountDisplayIds_.count(player.guid) > 0 &&
            !displayOverride;
        uint32_t instanceId = entitySpawner.getPlayerInstanceId(player.guid);
        if (instanceId == 0 && displayOverride) {
            instanceId = entitySpawner.getCreatureInstanceId(player.guid);
        }
        if (instanceId == 0) continue;

        glm::vec3 canonical = coords::serverToCanonical(glm::vec3(player.x, player.y, player.z));
        glm::vec3 renderPos = coords::canonicalToRender(canonical);
        float renderYaw = coords::serverToCanonicalYaw(player.orientation) + glm::radians(90.0f);

        uint32_t mountInstanceId = 0;
        auto mountGuidIt = replayMountGuids_.find(player.guid);
        if (player.mountDisplayId != 0 && mountGuidIt != replayMountGuids_.end()) {
            mountInstanceId = entitySpawner.getCreatureInstanceId(mountGuidIt->second);
            if (mountInstanceId != 0) {
                glm::vec3 mountRenderPos = coords::canonicalToRender(canonical);
                charRenderer->setInstancePosition(mountInstanceId, mountRenderPos);
                charRenderer->setInstanceRotation(mountInstanceId, glm::vec3(0.0f, 0.0f, renderYaw));
            }
        }

        if (mounted) {
            renderPos.z += mountInstanceId != 0
                ? replayMountRiderZOffset(*charRenderer, mountInstanceId)
                : kReplayMountFallbackRiderZOffset;
        }

        charRenderer->setInstancePosition(instanceId, renderPos);
        charRenderer->setInstanceRotation(instanceId, glm::vec3(0.0f, 0.0f, renderYaw));

        const bool moving = sampled.moving;
        auto lastIt = lastMoving_.find(player.guid);
        auto lastCombatIt = lastPlayerCombat_.find(player.guid);
        auto lastMountedIt = lastPlayerMounted_.find(player.guid);
        bool stateChanged = (lastIt == lastMoving_.end() || lastIt->second != moving) ||
                            (lastCombatIt == lastPlayerCombat_.end() || lastCombatIt->second != player.combat) ||
                            (lastMountedIt == lastPlayerMounted_.end() || lastMountedIt->second != mounted);
        if (stateChanged) {
            uint32_t anim = replayPlayerAnimation(player, moving, mounted, *charRenderer, instanceId);
            charRenderer->playAnimation(instanceId, anim, true);
            lastMoving_[player.guid] = moving;
            lastPlayerCombat_[player.guid] = player.combat;
            lastPlayerMounted_[player.guid] = mounted;
        }

        if (mountInstanceId != 0 && mountGuidIt != replayMountGuids_.end()) {
            auto lastMountIt = lastMountMoving_.find(mountGuidIt->second);
            bool mountStateChanged =
                lastMountIt == lastMountMoving_.end() || lastMountIt->second != moving;
            if (mountStateChanged) {
                uint32_t mountAnim = moving ? rendering::anim::RUN : rendering::anim::STAND;
                if (!charRenderer->hasAnimation(mountInstanceId, mountAnim)) {
                    mountAnim = rendering::anim::STAND;
                }
                charRenderer->playAnimation(mountInstanceId, mountAnim, true);
                lastMountMoving_[mountGuidIt->second] = moving;
            }
        }

        auto attackIt = attackPulses.find(player.guid);
        if (attackIt != attackPulses.end() && !mounted) {
            currentlyAttacking.insert(player.guid);
            const uint64_t pulseKey = replayAttackPulseKey(attackIt->second);
            auto lastPulseIt = lastReplayAttackPulseKey_.find(player.guid);
            if (lastPulseIt == lastReplayAttackPulseKey_.end() || lastPulseIt->second != pulseKey) {
                uint32_t attackAnim = replayPlayerAttackAnimation(player, *charRenderer, instanceId);
                if (attackAnim != 0) {
                    charRenderer->playAnimation(instanceId, attackAnim, false);
                    lastReplayAttackPulseKey_[player.guid] = pulseKey;
                }
            }
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
            lastCreatureCombat_[creature.guid] = creature.combat;
            lastCreatureDead_[creature.guid] = true;
            continue;
        }

        const bool moving = sampled.moving;
        auto lastIt = lastCreatureMoving_.find(creature.guid);
        auto lastCombatIt = lastCreatureCombat_.find(creature.guid);
        auto lastDeadIt = lastCreatureDead_.find(creature.guid);
        bool stateChanged = (lastIt == lastCreatureMoving_.end() || lastIt->second != moving) ||
                            (lastCombatIt == lastCreatureCombat_.end() || lastCombatIt->second != creature.combat) ||
                            (lastDeadIt == lastCreatureDead_.end() || lastDeadIt->second != false);
        if (stateChanged) {
            uint32_t anim = replayCreatureAnimation(creature, moving, *charRenderer, instanceId);
            charRenderer->playAnimation(instanceId, anim, true);
            lastCreatureMoving_[creature.guid] = moving;
            lastCreatureCombat_[creature.guid] = creature.combat;
            lastCreatureDead_[creature.guid] = false;
        }

        auto attackIt = attackPulses.find(creature.guid);
        if (attackIt != attackPulses.end()) {
            currentlyAttacking.insert(creature.guid);
            const uint64_t pulseKey = replayAttackPulseKey(attackIt->second);
            auto lastPulseIt = lastReplayAttackPulseKey_.find(creature.guid);
            if (lastPulseIt == lastReplayAttackPulseKey_.end() || lastPulseIt->second != pulseKey) {
                uint32_t attackAnim = replayCreatureAttackAnimation(*charRenderer, instanceId);
                if (attackAnim != 0) {
                    charRenderer->playAnimation(instanceId, attackAnim, false);
                    lastReplayAttackPulseKey_[creature.guid] = pulseKey;
                }
            }
        }
    }

    for (uint64_t guid : activeReplayAttackGuids_) {
        if (currentlyAttacking.count(guid) > 0) continue;
        lastMoving_.erase(guid);
        lastPlayerCombat_.erase(guid);
        lastCreatureMoving_.erase(guid);
        lastCreatureCombat_.erase(guid);
        lastCreatureDead_.erase(guid);
    }
    activeReplayAttackGuids_ = std::move(currentlyAttacking);
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
        case SDL_SCANCODE_COMMA:
            seekTargetOrCombatEvent(-1);
            break;
        case SDL_SCANCODE_PERIOD:
            seekTargetOrCombatEvent(1);
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
    ImGui::SameLine();
    if (ImGui::Button("< Event")) {
        seekTargetOrCombatEvent(-1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Event >")) {
        seekTargetOrCombatEvent(1);
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
