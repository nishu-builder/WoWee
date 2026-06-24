#pragma once

#include "core/godview_recording.hpp"

#include <glm/glm.hpp>
#include <SDL2/SDL.h>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {

namespace rendering { class Renderer; }
namespace game { class GameHandler; }

namespace core {

class EntitySpawner;

class GodviewReplay {
public:
    using Player = GodviewRecording::Player;
    using Creature = GodviewRecording::Creature;
    using Snapshot = GodviewRecording::Snapshot;
    using InterpolatedPlayer = GodviewRecording::InterpolatedPlayer;
    using InterpolatedCreature = GodviewRecording::InterpolatedCreature;

    struct CameraFocusTarget {
        uint64_t guid = 0;
        std::string name;
        glm::vec3 renderPosition{0.0f};
        bool hasTarget = false;
        uint64_t targetGuid = 0;
        std::string targetName;
        glm::vec3 targetRenderPosition{0.0f};
    };

    bool load(const std::string& path, std::string& error);
    bool empty() const { return recording_.empty(); }

    const std::string& path() const { return recording_.path(); }
    const Snapshot& firstSnapshot() const;
    uint64_t startMs() const { return startMs_; }
    uint64_t endMs() const { return endMs_; }
    double currentMs() const { return currentMs_; }
    float speed() const { return speed_; }
    bool paused() const { return paused_; }
    bool overlayVisible() const { return overlayVisible_; }
    bool cameraFollowEnabled() const { return cameraFollowEnabled_; }
    void setOverlayVisible(bool visible) { overlayVisible_ = visible; }
    void setCameraFollowEnabled(bool enabled);
    size_t snapshotCount() const { return recording_.snapshotCountForMap(mapId_); }
    uint32_t mapId() const { return mapId_; }

    const Player* firstPlayer() const;
    bool focusPlayerByQuery(const std::string& query);
    bool focusEventPlayer(GodviewRecording::EventKind kind);
    bool seekEvent(GodviewRecording::EventKind kind, int direction, bool includeCurrent = false);
    bool seekTargetOrCombatEvent(int direction, bool includeCurrent = false);
    std::optional<CameraFocusTarget> cameraFocusTarget() const;

    void start();
    void update(float deltaTime,
                game::GameHandler& gameHandler,
                EntitySpawner& entitySpawner,
                rendering::Renderer& renderer);
    void syncRender(game::GameHandler& gameHandler,
                    EntitySpawner& entitySpawner,
                    rendering::Renderer& renderer);
    void handleKeyDown(const SDL_KeyboardEvent& event);
    void renderOverlay();

private:
    void applyGameState(game::GameHandler& gameHandler,
                        EntitySpawner& entitySpawner,
                        const std::vector<InterpolatedPlayer>& players,
                        const std::vector<InterpolatedCreature>& creatures);
    void despawnMissingPlayers(game::GameHandler& gameHandler,
                               EntitySpawner& entitySpawner,
                               const std::unordered_set<uint64_t>& activeGuids);
    void despawnMissingCreatures(game::GameHandler& gameHandler,
                                 EntitySpawner& entitySpawner,
                                 const std::unordered_set<uint64_t>& activeGuids);
    uint64_t replayMountGuidForPlayer(uint64_t playerGuid);
    void syncReplayMountForPlayer(EntitySpawner& entitySpawner,
                                  const Player& player,
                                  const glm::vec3& canonicalPosition,
                                  float canonicalYaw);
    void despawnReplayMount(EntitySpawner& entitySpawner, uint64_t playerGuid);
    void setCurrentMs(double value);
    void focusNextPlayer(int direction);
    std::string focusedPlayerName() const;

    GodviewRecording recording_;
    uint32_t mapId_ = 0;
    uint64_t startMs_ = 0;
    uint64_t endMs_ = 0;
    double currentMs_ = 0.0;
    float speed_ = 1.0f;
    bool paused_ = false;
    bool overlayVisible_ = true;
    bool cameraFollowEnabled_ = false;
    uint64_t focusedPlayerGuid_ = 0;
    uint64_t focusedEventTargetGuid_ = 0;
    std::unordered_set<uint64_t> activePlayerGuids_;
    std::unordered_set<uint64_t> activeCreatureGuids_;
    std::unordered_map<uint64_t, bool> lastMoving_;
    std::unordered_map<uint64_t, bool> lastPlayerCombat_;
    std::unordered_map<uint64_t, bool> lastPlayerMounted_;
    std::unordered_map<uint64_t, bool> lastCreatureMoving_;
    std::unordered_map<uint64_t, bool> lastCreatureCombat_;
    std::unordered_map<uint64_t, bool> lastCreatureDead_;
    std::unordered_map<uint64_t, bool> lastMountMoving_;
    std::unordered_map<uint64_t, uint64_t> lastReplayAttackPulseKey_;
    std::unordered_set<uint64_t> activeReplayAttackGuids_;
    std::unordered_map<uint64_t, size_t> lastPlayerEquipmentHash_;
    std::unordered_set<uint64_t> displayOverridePlayerGuids_;
    std::unordered_map<uint64_t, uint32_t> displayOverridePlayerDisplayIds_;
    std::unordered_map<uint64_t, uint64_t> replayMountGuids_;
    std::unordered_map<uint64_t, uint32_t> replayMountDisplayIds_;
    std::unordered_set<uint64_t> replayMountSpawnQueued_;
    uint64_t nextReplayMountGuid_ = 0xF000000000000000ull;
};

} // namespace core
} // namespace wowee
