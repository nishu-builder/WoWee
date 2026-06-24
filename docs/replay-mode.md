# Coworld God-View Replay Mode

WoWee can run as an offline Coworld replay viewer with:

```bash
WOW_DATA_PATH=/path/to/extracted/classic-data ./wowee --replay /path/to/godview.jsonl
```

This mode does not connect to an auth server, world server, socket, or packet
stream. It reads a server-authoritative Coworld `godview_*.jsonl` recording and
drives WoWee's existing entity and renderer systems directly.

## Build And Data Spike

Validated locally on macOS with Homebrew dependencies:

```bash
brew install cmake glm vulkan-loader vulkan-headers shaderc unicorn stormlib molten-vk vulkan-tools

BREW=$(brew --prefix)
export PKG_CONFIG_PATH="$BREW/lib/pkgconfig:$(brew --prefix ffmpeg)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix vulkan-loader)/lib/pkgconfig:$(brew --prefix shaderc)/lib/pkgconfig"
cmake -S . -B build
cmake --build build --parallel "$(sysctl -n hw.logicalcpu)"
```

Classic 1.12 data was extracted from local MPQs with WoWee's asset extractor:

```bash
./build/bin/asset_extract /path/to/WoW-1.12/Data /path/to/wowee-data-classic
```

The extracted data directory also needs WoWee's `Data/expansions` and
`Data/opcodes` metadata. With the classic profile active, local validation loaded
79,887 manifest entries and rendered Azeroth/Elwynn terrain from map id `0`.

## Coworld Input Schema

The replay source is the Coworld recorder documented in
`coworld-vanilla-wow/docs/protocol/godview_recorder.md`, with the Python reader
at `coworld-vanilla-wow/backend/vmangos/replay/godview.py`.

Current v1 schema:

```json
{"t":1700000000000,"ms":532145,"map":1,"instance":0,
 "players":[{"guid":50,"name":"Drumz","level":3,"race":6,"class":7,
             "x":-3251.1,"y":-558.5,"z":34.97,"o":1.52,
             "hp":180,"maxhp":220,"target":2955,"combat":true}]}
```

`ms` is the server monotonic clock and is used for replay ordering, interpolation,
scrubbing, and speed control. `t` is wall-clock unix milliseconds. Real recorder
files can contain per-map snapshots interleaved by `ms`; the current WoWee CLI
opens the first map that has players and filters replay sampling to that map's
own timeline.

## Coordinate Verification

Coworld records VMaNGOS server coordinates. WoWee already has explicit conversion
helpers in `include/core/coordinates.hpp`:

- `serverToCanonical({x,y,z})` returns `{y,x,z}`.
- `canonicalToRender({x,y,z})` returns `{y,x,z}`.

Composed together, server `{x,y,z}` becomes render `{x,y,z}`. The swap is still
important internally because ADT tile lookup and game entities use WoWee's
canonical coordinate convention, but the final render-space terrain coordinates
line up directly with VMaNGOS positions.

Server orientation is converted through `serverToCanonicalYaw(o)`, then the
player model renderer applies its existing render yaw offset.

## Existing Entity And Scene Pipeline

Normal online play receives update packets in `GameHandler`, parses movement and
object fields through `EntityController`, and publishes spawn/move callbacks.

The important path for visible units is:

- `EntitySpawnCallbackHandler::setupCallbacks()` wires `GameHandler` callbacks
  into `EntitySpawner`.
- Player spawns call `EntitySpawner::queuePlayerSpawn(...)`.
- `EntitySpawner::update()` drains queues.
- `EntitySpawner::spawnOnlinePlayer(...)` resolves race/gender to an M2 path,
  loads skin and animation data, creates a `CharacterRenderer` instance, and
  stores the replay/network GUID to renderer instance mapping.
- Per-frame online updates live in `Application::update()` under
  `AppState::IN_GAME`; renderer movement is synchronized there and
  `Renderer::update()` advances camera, terrain streaming, world systems, and
  `CharacterRenderer`.
- Rendering lives in `Application::render()`, which calls
  `Renderer::renderWorld(...)` before UI/HUD rendering.

`WorldLoader::loadOnlineWorldTerrain(mapId, x, y, z)` is named for the online
entry path, but it already takes server coordinates, resolves `Map.dbc`, loads
the terrain/WMO/M2 scene for that map, and positions the camera from the same
coordinate helpers. Replay mode reuses it.

## Replay Mode Pipeline

`--replay <godview.jsonl>` is parsed in `src/main.cpp` before WoWee changes its
working directory, so relative paths are made absolute first.

`Application::startReplayMode()`:

- Loads the JSONL through `GodviewReplay`.
- Forces the classic expansion profile when available.
- Loads terrain for the first recorded map that has players, at that first
  player's recorded position.
- Switches the camera controller to fast free-fly observer mode.
- Puts `GameHandler` into an offline in-world state for labels/nameplates.
- Starts the replay clock and queues initial player model spawns.

During `AppState::IN_GAME`, replay mode follows a separate branch in
`Application::update()`:

- Advance replay time by `deltaTime * speed`, using the recorded `ms` clock.
- Sample the previous and next snapshots for the active map and interpolate
  position/orientation.
- Create/update `game::Player` entities with name, level, hp, maxhp, race, class,
  target fields, and combat state.
- Queue one renderable player model per active recorded player.
- Directly synchronize each player renderer instance from sampled replay state.
- Despawn players absent from the current authoritative snapshot.
- Keep terrain streaming enabled for the observer camera.

This branch does not call the world socket or normal opcode path. Normal client
behavior remains behind the existing non-replay path.

## Controls

- Space: play/pause.
- Left/Right: scrub 5 seconds; hold Shift for 30 seconds.
- Home/End: jump to recording start/end.
- `[` / `]`: decrease/increase replay speed.
- Overlay: play/pause buttons, start/end buttons, time slider, and speed slider.
- Camera: WoWee free-fly observer controls, with WoW movement speed disabled.

## Identity, Labels, Combat, And Targets

The current Coworld v1 stream records race/class but not gender, equipment, or
display IDs. Replay mode therefore:

- Uses race plus optional future `gender` to choose a player model, defaulting
  to male when `gender` is absent.
- Generates stable placeholder appearance bytes from GUID.
- Uses existing WoWee nameplate/entity fields for name and level labels.
- Writes target low/high fields when `target` is present.
- Uses run animation while interpolated movement is nonzero, unarmed-ready while
  idle in combat when available, and stand otherwise.

## Coworld Schema Follow-Up Note

For faithful 3D avatars, the server-side recorder should be extended to emit:

- `gender` per player.
- Player equipment/display IDs, or enough inventory display info to compose the
  same geosets and texture regions WoWee uses online.
- Player display IDs if the server has transformations that override race/gender.
- Creature snapshots with creature GUID, display ID, position/orientation, hp,
  target, combat, and movement state.

Until then, replay mode intentionally renders players with stable placeholders and
does not spawn creatures.

Producer-side tracking issue:
<https://github.com/Metta-AI/coworld-vanilla-wow/issues/82>

## Validation

Local validation completed:

- `cmake --build build --parallel "$(sysctl -n hw.logicalcpu)"` passed.
- `./build/bin/wowee --help` prints `Usage: ./build/bin/wowee [--replay <godview.jsonl>]`.
- `./build/bin/test_godview_recording` passed 48 assertions covering JSONL
  parsing, out-of-order server `ms` sorting, map-filtered interpolation,
  optional future `gender`, string/hex GUID values, and malformed-line errors.
- `./build/bin/test_entity` passed 71 assertions.
- `./build/bin/test_world_map_coordinate_projection` passed 30 assertions.
- `./build/bin/test_opcode_table` passed 18 assertions.
- `docs/examples/godview_smoke_elwynn.jsonl` loaded in replay mode.
- Runtime log confirmed classic metadata load, replay load, Azeroth terrain
  streaming, Elwynn zone entry, human/orc model composition, free-fly camera, and
  replay main loop entry.

No real `godview_*.jsonl` recording was present under the local Coworld checkout
or the running Coworld container at validation time; the container only had a
packet log in `/coworld-captures`. Phase-4 validation against a realm-captured
recording should use:

```bash
python -m backend.vmangos.replay.inspect_godview --latest ./local_data/coworld-captures
WOW_DATA_PATH=/path/to/wowee-data-classic ./build/bin/wowee --replay ./local_data/coworld-captures/godview_<timestamp>.jsonl
```

A macOS `screencapture` attempt produced the desktop background rather than the
Vulkan window contents, likely due to native window capture permissions; the
runtime verification above comes from WoWee logs.
