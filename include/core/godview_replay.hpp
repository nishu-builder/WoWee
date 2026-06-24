#pragma once

#include "core/godview_recording.hpp"

#include <SDL2/SDL.h>
#include <cstdint>
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
    void setOverlayVisible(bool visible) { overlayVisible_ = visible; }
    size_t snapshotCount() const { return recording_.snapshotCountForMap(mapId_); }
    uint32_t mapId() const { return mapId_; }

    const Player* firstPlayer() const;

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
    void setCurrentMs(double value);

    GodviewRecording recording_;
    uint32_t mapId_ = 0;
    uint64_t startMs_ = 0;
    uint64_t endMs_ = 0;
    double currentMs_ = 0.0;
    float speed_ = 1.0f;
    bool paused_ = false;
    bool overlayVisible_ = true;
    std::unordered_set<uint64_t> activePlayerGuids_;
    std::unordered_set<uint64_t> activeCreatureGuids_;
    std::unordered_map<uint64_t, bool> lastMoving_;
    std::unordered_map<uint64_t, bool> lastCreatureMoving_;
    std::unordered_map<uint64_t, size_t> lastPlayerEquipmentHash_;
};

} // namespace core
} // namespace wowee
