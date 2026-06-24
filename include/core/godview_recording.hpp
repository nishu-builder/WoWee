#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace core {

class GodviewRecording {
public:
    enum class EventKind {
        TargetOrCombat,
        Target,
        Combat,
        Death,
    };

    struct Equipment {
        uint8_t slot = 0;
        uint32_t itemId = 0;
        uint32_t displayId = 0;
        uint8_t inventoryType = 0;
        uint8_t itemClass = 0;
        uint8_t subclass = 0;
    };

    struct Player {
        uint64_t guid = 0;
        std::string rawGuid;
        std::string name;
        uint8_t level = 1;
        uint8_t race = 1;
        uint8_t playerClass = 1;
        uint8_t gender = 0;
        uint32_t displayId = 0;
        uint32_t nativeDisplayId = 0;
        uint32_t mountDisplayId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
        uint32_t hp = 1;
        uint32_t maxHp = 1;
        std::optional<uint64_t> targetGuid;
        bool combat = false;
        std::vector<Equipment> equipment;
    };

    struct Creature {
        uint64_t guid = 0;
        std::string rawGuid;
        uint32_t entry = 0;
        std::string name;
        uint8_t level = 1;
        uint8_t rank = 0;
        uint8_t type = 0;
        uint32_t displayId = 0;
        uint32_t nativeDisplayId = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
        uint32_t hp = 1;
        uint32_t maxHp = 1;
        std::optional<uint64_t> targetGuid;
        bool combat = false;
        bool dead = false;
    };

    struct ReplayEvent {
        std::string kind;
        uint64_t sourceGuid = 0;
        std::string sourceRawGuid;
        uint64_t targetGuid = 0;
        std::string targetRawGuid;
        uint32_t amount = 0;
        std::optional<uint32_t> overkill;
        std::optional<uint32_t> school;
        std::optional<uint32_t> spellId;
        std::optional<bool> critical;
    };

    struct Snapshot {
        uint64_t t = 0;
        uint32_t schema = 1;
        uint64_t ms = 0;
        uint32_t map = 0;
        uint32_t instance = 0;
        std::vector<Player> players;
        std::vector<Creature> creatures;
        std::vector<ReplayEvent> events;
        std::unordered_map<uint64_t, size_t> playerByGuid;
        std::unordered_map<uint64_t, size_t> creatureByGuid;
    };

    struct SnapshotPair {
        size_t prev = 0;
        size_t next = 0;
        float alpha = 0.0f;
        bool valid = false;
    };

    struct InterpolatedPlayer {
        Player player;
        bool moving = false;
    };

    struct InterpolatedCreature {
        Creature creature;
        bool moving = false;
    };

    bool load(const std::string& path, std::string& error);
    bool empty() const { return snapshots_.empty(); }

    const std::string& path() const { return path_; }
    const std::vector<Snapshot>& snapshots() const { return snapshots_; }
    const Snapshot& firstSnapshot() const { return snapshots_.front(); }
    const Snapshot* firstSnapshotForMap(uint32_t mapId) const;
    uint64_t startMs() const { return startMs_; }
    uint64_t endMs() const { return endMs_; }
    uint64_t startMsForMap(uint32_t mapId) const;
    uint64_t endMsForMap(uint32_t mapId) const;
    size_t snapshotCount() const { return snapshots_.size(); }
    size_t snapshotCountForMap(uint32_t mapId) const;
    size_t creatureCountForMap(uint32_t mapId) const;
    std::vector<uint32_t> mapIds() const;

    const Player* firstPlayer() const;
    const Player* firstPlayerForMap(uint32_t mapId) const;
    SnapshotPair findSnapshotPair(double currentMs, std::optional<uint32_t> mapId = std::nullopt) const;
    std::optional<uint64_t> findEventMs(double currentMs,
                                        std::optional<uint32_t> mapId,
                                        EventKind kind,
                                        int direction) const;
    std::optional<uint64_t> findTargetOrCombatEventMs(double currentMs,
                                                      std::optional<uint32_t> mapId,
                                                      int direction) const;
    std::vector<InterpolatedPlayer> samplePlayers(double currentMs,
                                                  std::optional<uint32_t> mapId = std::nullopt) const;
    std::vector<InterpolatedCreature> sampleCreatures(double currentMs,
                                                      std::optional<uint32_t> mapId = std::nullopt) const;

private:
    void rebuildIndexes();

    std::string path_;
    std::vector<Snapshot> snapshots_;
    std::unordered_map<uint32_t, std::vector<size_t>> snapshotIndicesByMap_;
    uint64_t startMs_ = 0;
    uint64_t endMs_ = 0;
};

} // namespace core
} // namespace wowee
