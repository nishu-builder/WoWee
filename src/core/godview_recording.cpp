#include "core/godview_recording.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace wowee {
namespace core {

namespace {
using Json = nlohmann::json;

std::string trim(std::string value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

uint64_t hashGuidString(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    hash |= 0x8000000000000000ull;
    return hash != 0 ? hash : 0x8000000000000001ull;
}

std::optional<uint64_t> parseUintString(const std::string& raw) {
    std::string value = trim(raw);
    if (value.empty()) return std::nullopt;

    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
    }

    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, base);
    if (end != value.c_str() && *end == '\0' && errno == 0) {
        return static_cast<uint64_t>(parsed);
    }
    return std::nullopt;
}

uint64_t guidFromJsonValue(const Json& value) {
    if (value.is_null()) return 0;
    if (value.is_number_unsigned()) return value.get<uint64_t>();
    if (value.is_number_integer()) {
        auto parsed = value.get<int64_t>();
        return parsed > 0 ? static_cast<uint64_t>(parsed) : 0;
    }
    if (value.is_string()) {
        std::string raw = value.get<std::string>();
        if (auto parsed = parseUintString(raw)) return *parsed;
        return hashGuidString(raw);
    }
    return 0;
}

uint64_t readU64(const Json& object, const char* key, uint64_t fallback = 0) {
    auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        int64_t value = it->get<int64_t>();
        return value >= 0 ? static_cast<uint64_t>(value) : fallback;
    }
    if (it->is_string()) {
        if (auto parsed = parseUintString(it->get<std::string>())) return *parsed;
    }
    return fallback;
}

uint32_t readU32(const Json& object, const char* key, uint32_t fallback = 0) {
    uint64_t value = readU64(object, key, fallback);
    return value <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(value) : fallback;
}

uint8_t readU8(const Json& object, const char* key, uint8_t fallback = 0) {
    uint64_t value = readU64(object, key, fallback);
    return value <= std::numeric_limits<uint8_t>::max() ? static_cast<uint8_t>(value) : fallback;
}

float readFloat(const Json& object, const char* key, float fallback = 0.0f) {
    auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_number()) return it->get<float>();
    if (it->is_string()) {
        std::string raw = it->get<std::string>();
        char* end = nullptr;
        errno = 0;
        const char* begin = raw.c_str();
        float parsed = std::strtof(begin, &end);
        if (end != begin && errno == 0 && std::isfinite(parsed)) {
            return parsed;
        }
    }
    return fallback;
}

bool readBool(const Json& object, const char* key, bool fallback = false) {
    auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_integer()) return it->get<int64_t>() != 0;
    if (it->is_string()) {
        std::string value = trim(it->get<std::string>());
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (value == "true" || value == "1" || value == "yes") return true;
        if (value == "false" || value == "0" || value == "no") return false;
    }
    return fallback;
}

std::string readString(const Json& object, const char* key, const std::string& fallback = {}) {
    auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<uint64_t>());
    if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
    return fallback;
}

uint64_t readEntityGuid(const Json& object) {
    auto rawIt = object.find("raw_guid");
    if (rawIt != object.end()) {
        uint64_t rawGuid = guidFromJsonValue(*rawIt);
        if (rawGuid != 0) return rawGuid;
    }

    auto guidIt = object.find("guid");
    return guidIt != object.end() ? guidFromJsonValue(*guidIt) : 0;
}

std::string readEntityGuidString(const Json& object, uint64_t guid) {
    auto rawIt = object.find("raw_guid");
    if (rawIt != object.end()) return readString(object, "raw_guid", std::to_string(guid));
    return readString(object, "guid", std::to_string(guid));
}

std::optional<uint64_t> readOptionalGuid(const Json& object, const char* rawKey, const char* lowKey) {
    auto rawIt = object.find(rawKey);
    if (rawIt != object.end()) {
        uint64_t rawGuid = guidFromJsonValue(*rawIt);
        if (rawGuid != 0) return rawGuid;
    }

    auto lowIt = object.find(lowKey);
    if (lowIt != object.end()) {
        uint64_t lowGuid = guidFromJsonValue(*lowIt);
        if (lowGuid != 0) return lowGuid;
    }

    return std::nullopt;
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float lerpAngle(float a, float b, float t) {
    float delta = coords::normalizeAngleRad(b - a);
    return coords::normalizeAngleRad(a + delta * t);
}

bool hasReplayEvent(const GodviewRecording::Snapshot& snapshot,
                    GodviewRecording::EventKind kind) {
    for (const auto& player : snapshot.players) {
        if (player.combat) return true;
        if (kind == GodviewRecording::EventKind::TargetOrCombat && player.targetGuid) return true;
    }
    for (const auto& creature : snapshot.creatures) {
        if (creature.combat) return true;
        if (kind == GodviewRecording::EventKind::TargetOrCombat && creature.targetGuid) return true;
    }
    return false;
}

} // namespace

bool GodviewRecording::load(const std::string& path, std::string& error) {
    path_ = path;
    snapshots_.clear();
    snapshotIndicesByMap_.clear();
    startMs_ = 0;
    endMs_ = 0;

    std::ifstream file(path);
    if (!file.is_open()) {
        error = "failed to open replay file: " + path;
        return false;
    }

    std::string line;
    size_t lineNo = 0;
    size_t playerCount = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        if (trim(line).empty()) continue;

        Json doc;
        try {
            doc = Json::parse(line);
        } catch (const Json::parse_error& e) {
            std::ostringstream oss;
            oss << "JSON parse error at line " << lineNo << ": " << e.what();
            error = oss.str();
            return false;
        }
        if (!doc.is_object()) {
            std::ostringstream oss;
            oss << "line " << lineNo << " is not a JSON object";
            error = oss.str();
            return false;
        }

        Snapshot snapshot;
        snapshot.t = readU64(doc, "t");
        snapshot.schema = readU32(doc, "schema", 1);
        snapshot.ms = readU64(doc, "ms", snapshot.t);
        snapshot.map = readU32(doc, "map");
        snapshot.instance = readU32(doc, "instance");

        auto playersIt = doc.find("players");
        if (playersIt != doc.end() && playersIt->is_array()) {
            for (const auto& playerDoc : *playersIt) {
                if (!playerDoc.is_object()) continue;

                Player player;
                player.guid = readEntityGuid(playerDoc);
                if (player.guid == 0) continue;
                player.rawGuid = readEntityGuidString(playerDoc, player.guid);
                player.name = readString(playerDoc, "name", "Player");
                player.level = std::max<uint8_t>(1, readU8(playerDoc, "level", 1));
                player.race = std::max<uint8_t>(1, readU8(playerDoc, "race", 1));
                player.playerClass = std::max<uint8_t>(1, readU8(playerDoc, "class", 1));
                player.gender = std::min<uint8_t>(1, readU8(playerDoc, "gender", 0));
                player.displayId = readU32(playerDoc, "display_id");
                player.nativeDisplayId = readU32(playerDoc, "native_display_id");
                player.mountDisplayId = readU32(playerDoc, "mount_display_id");
                player.x = readFloat(playerDoc, "x");
                player.y = readFloat(playerDoc, "y");
                player.z = readFloat(playerDoc, "z");
                player.orientation = readFloat(playerDoc, "o");
                player.maxHp = std::max<uint32_t>(1, readU32(playerDoc, "maxhp", 1));
                player.hp = std::min(readU32(playerDoc, "hp", player.maxHp), player.maxHp);
                player.combat = readBool(playerDoc, "combat", false);
                player.targetGuid = readOptionalGuid(playerDoc, "target_raw", "target");

                auto equipmentIt = playerDoc.find("equipment");
                if (equipmentIt != playerDoc.end() && equipmentIt->is_array()) {
                    for (const auto& equipmentDoc : *equipmentIt) {
                        if (!equipmentDoc.is_object()) continue;

                        Equipment equipment;
                        equipment.slot = readU8(equipmentDoc, "slot");
                        if (equipment.slot >= 19) continue;
                        equipment.itemId = readU32(equipmentDoc, "item_id");
                        equipment.displayId = readU32(equipmentDoc, "display_id");
                        equipment.inventoryType = readU8(equipmentDoc, "inventory_type");
                        equipment.itemClass = readU8(equipmentDoc, "class");
                        equipment.subclass = readU8(equipmentDoc, "subclass");
                        if (equipment.displayId == 0 && equipment.itemId == 0) continue;
                        player.equipment.push_back(equipment);
                    }
                }

                snapshot.playerByGuid[player.guid] = snapshot.players.size();
                snapshot.players.push_back(std::move(player));
            }
        }

        auto creaturesIt = doc.find("creatures");
        if (creaturesIt != doc.end() && creaturesIt->is_array()) {
            for (const auto& creatureDoc : *creaturesIt) {
                if (!creatureDoc.is_object()) continue;

                Creature creature;
                creature.guid = readEntityGuid(creatureDoc);
                if (creature.guid == 0) continue;
                creature.rawGuid = readEntityGuidString(creatureDoc, creature.guid);
                creature.entry = readU32(creatureDoc, "entry");
                creature.name = readString(creatureDoc, "name", "Creature");
                creature.level = std::max<uint8_t>(1, readU8(creatureDoc, "level", 1));
                creature.rank = readU8(creatureDoc, "rank");
                creature.type = readU8(creatureDoc, "type");
                creature.displayId = readU32(creatureDoc, "display_id");
                creature.nativeDisplayId = readU32(creatureDoc, "native_display_id");
                creature.x = readFloat(creatureDoc, "x");
                creature.y = readFloat(creatureDoc, "y");
                creature.z = readFloat(creatureDoc, "z");
                creature.orientation = readFloat(creatureDoc, "o");
                creature.maxHp = std::max<uint32_t>(1, readU32(creatureDoc, "maxhp", 1));
                creature.hp = std::min(readU32(creatureDoc, "hp", creature.maxHp), creature.maxHp);
                creature.targetGuid = readOptionalGuid(creatureDoc, "target_raw", "target");
                creature.combat = readBool(creatureDoc, "combat", false);
                creature.dead = readBool(creatureDoc, "dead", false);

                snapshot.creatureByGuid[creature.guid] = snapshot.creatures.size();
                snapshot.creatures.push_back(std::move(creature));
            }
        }

        playerCount += snapshot.players.size();
        snapshots_.push_back(std::move(snapshot));
    }

    if (snapshots_.empty()) {
        error = "replay file contains no snapshots";
        return false;
    }
    if (playerCount == 0) {
        error = "replay file contains no players";
        return false;
    }

    std::stable_sort(snapshots_.begin(), snapshots_.end(),
                     [](const Snapshot& a, const Snapshot& b) { return a.ms < b.ms; });
    rebuildIndexes();

    startMs_ = snapshots_.front().ms;
    endMs_ = snapshots_.back().ms;

    LOG_INFO("Godview replay loaded: ", path_, " snapshots=", snapshots_.size(),
             " ms=", startMs_, "-", endMs_);
    return true;
}

void GodviewRecording::rebuildIndexes() {
    snapshotIndicesByMap_.clear();
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        auto& snapshot = snapshots_[i];
        snapshot.playerByGuid.clear();
        for (size_t playerIndex = 0; playerIndex < snapshot.players.size(); ++playerIndex) {
            snapshot.playerByGuid[snapshot.players[playerIndex].guid] = playerIndex;
        }
        snapshot.creatureByGuid.clear();
        for (size_t creatureIndex = 0; creatureIndex < snapshot.creatures.size(); ++creatureIndex) {
            snapshot.creatureByGuid[snapshot.creatures[creatureIndex].guid] = creatureIndex;
        }
        snapshotIndicesByMap_[snapshot.map].push_back(i);
    }
}

const GodviewRecording::Snapshot* GodviewRecording::firstSnapshotForMap(uint32_t mapId) const {
    auto it = snapshotIndicesByMap_.find(mapId);
    if (it == snapshotIndicesByMap_.end() || it->second.empty()) return nullptr;
    return &snapshots_[it->second.front()];
}

uint64_t GodviewRecording::startMsForMap(uint32_t mapId) const {
    auto snapshot = firstSnapshotForMap(mapId);
    return snapshot ? snapshot->ms : startMs_;
}

uint64_t GodviewRecording::endMsForMap(uint32_t mapId) const {
    auto it = snapshotIndicesByMap_.find(mapId);
    if (it == snapshotIndicesByMap_.end() || it->second.empty()) return endMs_;
    return snapshots_[it->second.back()].ms;
}

size_t GodviewRecording::snapshotCountForMap(uint32_t mapId) const {
    auto it = snapshotIndicesByMap_.find(mapId);
    return it == snapshotIndicesByMap_.end() ? 0 : it->second.size();
}

size_t GodviewRecording::creatureCountForMap(uint32_t mapId) const {
    auto it = snapshotIndicesByMap_.find(mapId);
    if (it == snapshotIndicesByMap_.end()) return 0;

    size_t count = 0;
    for (size_t index : it->second) {
        count = std::max(count, snapshots_[index].creatures.size());
    }
    return count;
}

std::vector<uint32_t> GodviewRecording::mapIds() const {
    std::vector<uint32_t> ids;
    ids.reserve(snapshotIndicesByMap_.size());
    for (const auto& entry : snapshotIndicesByMap_) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

const GodviewRecording::Player* GodviewRecording::firstPlayer() const {
    for (const auto& snapshot : snapshots_) {
        if (!snapshot.players.empty()) return &snapshot.players.front();
    }
    return nullptr;
}

const GodviewRecording::Player* GodviewRecording::firstPlayerForMap(uint32_t mapId) const {
    auto it = snapshotIndicesByMap_.find(mapId);
    if (it == snapshotIndicesByMap_.end()) return nullptr;
    for (size_t index : it->second) {
        const auto& snapshot = snapshots_[index];
        if (!snapshot.players.empty()) return &snapshot.players.front();
    }
    return nullptr;
}

GodviewRecording::SnapshotPair GodviewRecording::findSnapshotPair(double currentMs,
                                                                  std::optional<uint32_t> mapId) const {
    SnapshotPair pair;
    if (snapshots_.empty()) return pair;

    std::vector<size_t> allIndices;
    const std::vector<size_t>* indices = nullptr;
    if (mapId) {
        auto it = snapshotIndicesByMap_.find(*mapId);
        if (it == snapshotIndicesByMap_.end() || it->second.empty()) return pair;
        indices = &it->second;
    } else {
        allIndices.reserve(snapshots_.size());
        for (size_t i = 0; i < snapshots_.size(); ++i) {
            allIndices.push_back(i);
        }
        indices = &allIndices;
    }

    pair.valid = true;
    if (indices->size() == 1 || currentMs <= static_cast<double>(snapshots_[indices->front()].ms)) {
        pair.prev = indices->front();
        pair.next = pair.prev;
        return pair;
    }
    if (currentMs >= static_cast<double>(snapshots_[indices->back()].ms)) {
        pair.prev = indices->back();
        pair.next = pair.prev;
        return pair;
    }

    auto it = std::upper_bound(indices->begin(), indices->end(), currentMs,
                               [this](double ms, size_t snapshotIndex) {
                                   return ms < static_cast<double>(snapshots_[snapshotIndex].ms);
                               });
    pair.next = *it;
    pair.prev = it == indices->begin() ? pair.next : *(it - 1);
    const uint64_t prevMs = snapshots_[pair.prev].ms;
    const uint64_t nextMs = snapshots_[pair.next].ms;
    if (nextMs > prevMs) {
        pair.alpha = static_cast<float>((currentMs - static_cast<double>(prevMs)) /
                                        static_cast<double>(nextMs - prevMs));
        pair.alpha = std::clamp(pair.alpha, 0.0f, 1.0f);
    }
    return pair;
}

std::optional<uint64_t> GodviewRecording::findEventMs(double currentMs,
                                                      std::optional<uint32_t> mapId,
                                                      EventKind kind,
                                                      int direction) const {
    if (snapshots_.empty()) return std::nullopt;

    std::vector<size_t> allIndices;
    const std::vector<size_t>* indices = nullptr;
    if (mapId) {
        auto it = snapshotIndicesByMap_.find(*mapId);
        if (it == snapshotIndicesByMap_.end() || it->second.empty()) return std::nullopt;
        indices = &it->second;
    } else {
        allIndices.reserve(snapshots_.size());
        for (size_t i = 0; i < snapshots_.size(); ++i) {
            allIndices.push_back(i);
        }
        indices = &allIndices;
    }

    constexpr double kSameSnapshotSlackMs = 0.5;
    if (direction < 0) {
        for (auto it = indices->rbegin(); it != indices->rend(); ++it) {
            const Snapshot& snapshot = snapshots_[*it];
            if (static_cast<double>(snapshot.ms) >= currentMs - kSameSnapshotSlackMs) continue;
            if (hasReplayEvent(snapshot, kind)) return snapshot.ms;
        }
    } else {
        for (size_t index : *indices) {
            const Snapshot& snapshot = snapshots_[index];
            if (static_cast<double>(snapshot.ms) <= currentMs + kSameSnapshotSlackMs) continue;
            if (hasReplayEvent(snapshot, kind)) return snapshot.ms;
        }
    }

    return std::nullopt;
}

std::optional<uint64_t> GodviewRecording::findTargetOrCombatEventMs(double currentMs,
                                                                    std::optional<uint32_t> mapId,
                                                                    int direction) const {
    return findEventMs(currentMs, mapId, EventKind::TargetOrCombat, direction);
}

std::vector<GodviewRecording::InterpolatedPlayer> GodviewRecording::samplePlayers(
    double currentMs,
    std::optional<uint32_t> mapId) const {
    std::vector<InterpolatedPlayer> result;
    SnapshotPair pair = findSnapshotPair(currentMs, mapId);
    if (!pair.valid) return result;

    const Snapshot& prev = snapshots_[pair.prev];
    const Snapshot& next = snapshots_[pair.next];
    result.reserve(prev.players.size());

    for (const Player& previousPlayer : prev.players) {
        InterpolatedPlayer sampled;
        sampled.player = previousPlayer;

        auto nextPlayerIt = next.playerByGuid.find(previousPlayer.guid);
        if (nextPlayerIt != next.playerByGuid.end() && pair.next != pair.prev) {
            const Player& nextPlayer = next.players[nextPlayerIt->second];
            sampled.player.x = lerp(previousPlayer.x, nextPlayer.x, pair.alpha);
            sampled.player.y = lerp(previousPlayer.y, nextPlayer.y, pair.alpha);
            sampled.player.z = lerp(previousPlayer.z, nextPlayer.z, pair.alpha);
            sampled.player.orientation = lerpAngle(previousPlayer.orientation, nextPlayer.orientation, pair.alpha);
            sampled.player.hp = static_cast<uint32_t>(std::round(lerp(
                static_cast<float>(previousPlayer.hp),
                static_cast<float>(nextPlayer.hp),
                pair.alpha)));
            sampled.player.maxHp = pair.alpha < 0.5f ? previousPlayer.maxHp : nextPlayer.maxHp;
            sampled.player.level = pair.alpha < 0.5f ? previousPlayer.level : nextPlayer.level;
            sampled.player.combat = pair.alpha < 0.5f ? previousPlayer.combat : nextPlayer.combat;
            sampled.player.targetGuid = pair.alpha < 0.5f ? previousPlayer.targetGuid : nextPlayer.targetGuid;

            const float dx = nextPlayer.x - previousPlayer.x;
            const float dy = nextPlayer.y - previousPlayer.y;
            const float dz = nextPlayer.z - previousPlayer.z;
            sampled.moving = (dx * dx + dy * dy + dz * dz) > (0.03f * 0.03f);
        }

        result.push_back(std::move(sampled));
    }

    return result;
}

std::vector<GodviewRecording::InterpolatedCreature> GodviewRecording::sampleCreatures(
    double currentMs,
    std::optional<uint32_t> mapId) const {
    std::vector<InterpolatedCreature> result;
    SnapshotPair pair = findSnapshotPair(currentMs, mapId);
    if (!pair.valid) return result;

    const Snapshot& prev = snapshots_[pair.prev];
    const Snapshot& next = snapshots_[pair.next];
    result.reserve(prev.creatures.size());

    for (const Creature& previousCreature : prev.creatures) {
        InterpolatedCreature sampled;
        sampled.creature = previousCreature;

        auto nextCreatureIt = next.creatureByGuid.find(previousCreature.guid);
        if (nextCreatureIt != next.creatureByGuid.end() && pair.next != pair.prev) {
            const Creature& nextCreature = next.creatures[nextCreatureIt->second];
            sampled.creature.x = lerp(previousCreature.x, nextCreature.x, pair.alpha);
            sampled.creature.y = lerp(previousCreature.y, nextCreature.y, pair.alpha);
            sampled.creature.z = lerp(previousCreature.z, nextCreature.z, pair.alpha);
            sampled.creature.orientation = lerpAngle(previousCreature.orientation,
                                                     nextCreature.orientation,
                                                     pair.alpha);
            sampled.creature.hp = static_cast<uint32_t>(std::round(lerp(
                static_cast<float>(previousCreature.hp),
                static_cast<float>(nextCreature.hp),
                pair.alpha)));
            sampled.creature.maxHp = pair.alpha < 0.5f ? previousCreature.maxHp : nextCreature.maxHp;
            sampled.creature.level = pair.alpha < 0.5f ? previousCreature.level : nextCreature.level;
            sampled.creature.combat = pair.alpha < 0.5f ? previousCreature.combat : nextCreature.combat;
            sampled.creature.dead = pair.alpha < 0.5f ? previousCreature.dead : nextCreature.dead;
            sampled.creature.targetGuid = pair.alpha < 0.5f ? previousCreature.targetGuid : nextCreature.targetGuid;

            const float dx = nextCreature.x - previousCreature.x;
            const float dy = nextCreature.y - previousCreature.y;
            const float dz = nextCreature.z - previousCreature.z;
            sampled.moving = !sampled.creature.dead &&
                             (dx * dx + dy * dy + dz * dz) > (0.03f * 0.03f);
        }

        result.push_back(std::move(sampled));
    }

    return result;
}

} // namespace core
} // namespace wowee
