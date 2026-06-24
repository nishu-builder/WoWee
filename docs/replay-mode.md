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

WoWee supports the original v1 player-only schema and Coworld recorder v2. v2
adds raw packed GUIDs, player display/equipment identity, and loaded nearby
creatures:

```json
{"t":1700000000000,"schema":2,"ms":532145,"map":1,"instance":0,
 "players":[{"guid":50,"raw_guid":"0x10000000032","name":"Drumz",
             "level":3,"race":6,"class":7,"gender":0,
             "display_id":20582,"native_display_id":20582,"mount_display_id":0,
             "x":-3251.1,"y":-558.5,"z":34.97,"o":1.52,
             "hp":180,"maxhp":220,"target":2955,"target_raw":"0x20000000b8b",
             "combat":true,
             "equipment":[{"slot":15,"item_id":36,"display_id":5195,
                           "inventory_type":13,"class":2,"subclass":7}]}],
 "creatures":[{"guid":2955,"raw_guid":"0x20000000b8b","entry":2955,
               "name":"Plainstrider","level":6,"rank":0,"type":1,
               "display_id":390,"native_display_id":390,
               "x":-3310.0,"y":-570.0,"z":35.1,"o":2.4,
               "hp":120,"maxhp":120,"target":0,"target_raw":0,
               "combat":false,"dead":false}]}
```

`ms` is the server monotonic clock and is used for replay ordering, interpolation,
scrubbing, and speed control. `t` is wall-clock unix milliseconds. Real recorder
files can contain per-map snapshots interleaved by `ms`; the current WoWee CLI
opens the first map that has players and filters replay sampling to that map's
own timeline. When `raw_guid` / `target_raw` are present, WoWee uses them as the
replay entity identity; v1 recordings fall back to the original `guid` / `target`
fields.

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
  player and creature position/orientation.
- Create/update `game::Player` entities with name, level, hp, maxhp, race, class,
  display fields, target fields, and combat state.
- Create/update `game::Unit` entities for recorded creatures with entry, name,
  level, display ID, hp, target, combat, and dead state.
- Queue one renderable player model per active recorded player.
- Queue recorded player equipment through WoWee's existing online equipment
  compositor when the equipment hash changes.
- Queue one renderable creature model per active recorded creature with a
  display ID.
- Directly synchronize player and creature renderer instances from sampled
  replay state.
- Despawn players and creatures absent from the current authoritative snapshot.
- Keep terrain streaming enabled for the observer camera.

This branch does not call the world socket or normal opcode path. Normal client
behavior remains behind the existing non-replay path.

## Controls

- Space: play/pause.
- Left/Right: scrub 5 seconds; hold Shift for 30 seconds.
- Comma/Period: jump to the previous/next target-or-combat snapshot.
- Home/End: jump to recording start/end.
- `[` / `]`: decrease/increase replay speed.
- `H`: hide/show the compact replay overlay.
- `F`: toggle the replay follow camera for the focused recorded player.
- `Tab` / Shift+`Tab`: focus the next/previous recorded player and enable the
  follow camera.
- Overlay: play/pause buttons, start/end buttons, event buttons, time slider,
  and speed slider.
- Camera: WoWee free-fly observer controls, with WoW movement speed disabled.
- UI: offline replay keeps nameplates, minimap markers, chat bubbles, and the
  compact replay overlay, but suppresses normal gameplay panels such as player
  frames, chat dock, action bars, bags, quest windows, and spellbook windows.

## Identity, Labels, Combat, And Targets

Replay mode now uses Coworld v2 identity fields when available:

- Uses race plus `gender` to choose a player model, defaulting to male for v1
  recordings when `gender` is absent.
- Resolves `display_id` / `native_display_id` through WoWee's
  `CreatureDisplayInfo` and `CreatureDisplayInfoExtra` lookups when available,
  using the server display row for player race, gender, skin, face, hair, and
  facial hair at spawn time.
- Falls back to stable placeholder face/hair appearance bytes from GUID for v1
  recordings or display IDs not present in the local DBCs.
- Applies recorded player equipment display IDs through WoWee's existing armor
  and weapon compositor.
- Uses equipment display IDs from the resolved player display row as a fallback
  only when the recording has no explicit equipment array.
- Uses non-humanoid player `display_id` / `native_display_id` values as a
  replay-only display-model override, so shapeshifts and transformations can
  render as their recorded server model while keeping player labels and target
  state. Humanoid display IDs still feed race, gender, skin, hair, and equipment
  appearance.
- Sets player mount display fields for UI/state fidelity. Full rider-on-mount
  composition still needs a dedicated mount attachment path.
- Uses creature display IDs to spawn nearby recorded mobs/pets as renderable
  `game::Unit` entities.
- Uses existing WoWee nameplate/entity fields for name and level labels.
- Starts replay in a high observer camera centered over the first snapshot's
  recorded players and creatures, with gravity and default floor-snap disabled
  so the camera stays in a true god-view position.
- Can lock the observer camera to a recorded player from the replay overlay,
  `F`/`Tab` controls, or `WOWEE_REPLAY_FOCUS_PLAYER=first|name|guid` for
  deterministic inspection captures.
- Can start at the first target-or-combat snapshot with
  `WOWEE_REPLAY_START_EVENT=1`, which is useful for automated screenshot smokes
  that should land on action without hardcoding a timestamp.
- When the focused player has a recorded target that is also present in the
  sampled frame, the follow camera frames the player-target midpoint and backs
  out far enough to keep the engagement readable.
- Forces offline replay nameplates on for recorded players, keeps player labels
  visible from the high observer camera, and limits default creature labels to
  selected, combat, or target-bearing units so dense captures remain readable.
- Adds a compact replay-only backing behind name/sub-label text so labels remain
  legible in god-view screenshots over bright terrain.
- Marks replay nameplates with a red border and compact `combat` sublabel when
  the recording says the unit is in combat.
- Marks replay nameplates with an orange border and a compact `target: Name`
  sublabel when the recording contains a current target GUID.
- Shows units targeted by a recorded player/creature even when they are not
  otherwise selected, and marks them with a compact `targeted` sublabel.
- Separates close replay target/source nameplates in screen space when the two
  labels would otherwise overlap.
- Draws a replay-only target tether with a dark silhouette, arrow head, endpoint
  reticle, and high-contrast combat color between visible target-bearing units
  and their current target so god-view captures show engagement relationships at
  a glance.
- Caps very long creature-source tethers so distant incidental target links do
  not cut across clean god-view captures.
- Writes target low/high fields from `target_raw` when present, falling back to
  v1 `target`.
- Uses run animation while interpolated movement is nonzero, unarmed-ready while
  idle in combat when available, stand otherwise, and death animation when a
  creature snapshot is marked dead.

## Validation

For automated replay visual smoke tests, set `WOWEE_REPLAY_SCREENSHOT_PATH` to
write a one-shot Vulkan screenshot after replay mode reaches the main loop:

```bash
WOWEE_REPLAY_SCREENSHOT_PATH=/tmp/wowee-replay.png \
WOWEE_REPLAY_SCREENSHOT_EXIT=1 \
WOW_DATA_PATH=/path/to/extracted/classic-data \
./build/bin/wowee --replay /path/to/godview.jsonl
```

`WOWEE_REPLAY_SCREENSHOT_MS` optionally captures at a deterministic replay
timestamp, in milliseconds from the first recording snapshot. Values that fall
inside the recording's absolute `ms` range are treated as absolute server
milliseconds. `WOWEE_REPLAY_SCREENSHOT_FRAMES` still controls how many rendered
frames to wait before capture when no timestamp is supplied; it defaults to
`120`.

Set `WOWEE_REPLAY_HIDE_OVERLAY=1` to hide only the replay control overlay. Set
`WOWEE_REPLAY_CLEAN_CAPTURE=1` to hide the replay overlay plus minimap chrome for
clean captures while keeping replay nameplates and target cues visible. The replay
overlay can also be toggled interactively with `H`.

Set `WOWEE_REPLAY_FOCUS_PLAYER=first` or a recorded player name/guid to start with
the observer camera following that player. This is useful with
`WOWEE_REPLAY_SCREENSHOT_MS` when comparing captures across replay changes.

The replay screenshot hook records from the active Vulkan frame before present,
so it can be used in automated smoke tests without relying on desktop capture.

Local validation completed:

- `cmake --build build --parallel "$(sysctl -n hw.logicalcpu)"` passed.
- `./build/bin/wowee --help` prints `Usage: ./build/bin/wowee [--replay <godview.jsonl>]`.
- `./build/bin/test_godview_recording` passed 73 assertions covering JSONL
  parsing, out-of-order server `ms` sorting, map-filtered interpolation, v1/v2
  GUID handling, recorded equipment, creature interpolation, optional `gender`,
  string/hex GUID values, and malformed-line errors.
- `./build/bin/test_entity` passed 71 assertions.
- `./build/bin/test_world_map_coordinate_projection` passed 30 assertions.
- `./build/bin/test_opcode_table` passed 18 assertions.
- `docs/examples/godview_smoke_elwynn.jsonl` loaded in replay mode.
- Runtime log confirmed classic metadata load, replay load, Azeroth terrain
  streaming, Elwynn zone entry, human/orc model composition, free-fly camera, and
  replay main loop entry.
- Real v2 Coworld replay smoke:
  `WOWEE_REPLAY_SCREENSHOT_PATH=/Users/nishadsingh/repos/wow/WoWee/build/bin/wowee_replay_godview_clean_labels.png`
  with `WOWEE_REPLAY_SCREENSHOT_EXIT=1` wrote a nonblank 1280x720 PNG from
  `godview_1782289866.jsonl`, loaded Kalimdor terrain, rendered 30k M2
  instances, opened in a high centered observer view, kept the recorded player
  label readable, and exited successfully.

Realm-generated Coworld validation completed with a recorder-enabled VMaNGOS
container from current `coworld-vanilla-wow` patches:

- Recording:
  `/Users/nishadsingh/repos/wow/coworld-vanilla-wow/local_data/godview-proof/captures/godview_1782281379.jsonl`.
- Coworld inspector:
  `snapshots=45`, `maps=[1]`, `distinct players=1`, `ms-clock span=23710 ms`,
  `Replayhunt` moved `103.6` yards.
- First snapshot:
  map `1`, race `2`, class `7`, position
  `(-619.744812, -4252.344238, 40.591400)`.
- Last snapshot:
  position `(-686.551880, -4267.936523, 40.276199)`, target `7975`.
- WoWee runtime replay:
  `Godview replay active map: 1`, `Starting replay mode: map=1 firstPlayer=Replayhunt`,
  `Loading online world terrain for map 'Kalimdor' (ID 1)`,
  `Online terrain streaming complete: 49 tiles loaded`,
  `Free-fly camera enabled`, `Composite: base layer ... OrcMaleSkin00_01.blp`,
  `Replay mode ready`, and terrain diagnostics near the recorded coordinates.

Commands used for real-recording validation:

```bash
uv run python -m backend.vmangos.replay.inspect_godview --latest ./local_data/godview-proof/captures
WOW_DATA_PATH=/Users/nishadsingh/repos/wow/wowee-data-classic ./build/bin/wowee --replay /Users/nishadsingh/repos/wow/coworld-vanilla-wow/local_data/godview-proof/captures/godview_1782281379.jsonl
```

A macOS `screencapture` attempt produced the desktop background rather than the
Vulkan window contents, likely due to native window capture permissions; the
runtime verification above comes from WoWee logs.
