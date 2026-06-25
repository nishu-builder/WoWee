#!/usr/bin/env python3
"""
Run a replay screenshot smoke test and validate the resulting PNG.

The smoke path uses WoWee's replay screenshot environment hooks, then parses the
PNG with only the Python standard library to catch blank or stale captures.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class PngError(ValueError):
    pass


class ReplayError(ValueError):
    pass


def guid_is_present(value: object) -> bool:
    if value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value != 0
    if isinstance(value, str):
        raw = value.strip()
        if not raw:
            return False
        try:
            return int(raw, 0) != 0
        except ValueError:
            return True
    return False


def normalize_event(value: str | None) -> str | None:
    if not value:
        return None
    lowered = value.strip().lower()
    if not lowered or lowered[0] in {"0", "f", "n"}:
        return None
    if lowered == "target":
        return "target"
    if lowered == "combat":
        return "combat"
    if lowered == "damage":
        return "damage"
    if lowered in {"death", "dead"}:
        return "death"
    return "target-or-combat"


def snapshot_has_event(snapshot: dict[str, object], event: str) -> bool:
    replay_events = snapshot.get("events")
    if isinstance(replay_events, list):
        for replay_event in replay_events:
            if not isinstance(replay_event, dict):
                continue
            kind = str(replay_event.get("kind", "")).lower()
            if event == "death" and kind == "death":
                return True
            if event == "damage" and kind == "damage":
                return True
            if event == "combat" and kind in {"combat", "damage"}:
                return True
            if event == "target" and kind == "target":
                return True
            if event == "target-or-combat" and kind in {"target", "combat", "damage", "death"}:
                return True

    if event == "damage":
        return False

    players = snapshot.get("players")
    if not isinstance(players, list):
        players = []
    creatures = snapshot.get("creatures")
    if not isinstance(creatures, list):
        creatures = []

    if event == "death":
        for player in players:
            if isinstance(player, dict) and player.get("hp") == 0:
                return True
        for creature in creatures:
            if isinstance(creature, dict) and (creature.get("dead") is True or creature.get("hp") == 0):
                return True
        return False

    for entity in [*players, *creatures]:
        if not isinstance(entity, dict):
            continue
        has_target = guid_is_present(entity.get("target_raw")) or guid_is_present(entity.get("target"))
        in_combat = entity.get("combat") is True
        if event != "target" and in_combat:
            return True
        if event != "combat" and has_target:
            return True
    return False


def preflight_replay_event(path: Path, event: str) -> tuple[int, int]:
    active_map: int | None = None
    first_matching_ms: int | None = None

    with path.open("r", encoding="utf-8") as file:
        for line_number, line in enumerate(file, start=1):
            if not line.strip():
                continue
            try:
                snapshot = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ReplayError(f"JSON parse error at line {line_number}: {exc}") from exc
            if not isinstance(snapshot, dict):
                raise ReplayError(f"line {line_number} is not a JSON object")

            players = snapshot.get("players")
            if active_map is None and isinstance(players, list) and players:
                active_map = int(snapshot.get("map", 0) or 0)
            if active_map is None or int(snapshot.get("map", 0) or 0) != active_map:
                continue
            if snapshot_has_event(snapshot, event):
                first_matching_ms = int(snapshot.get("ms", snapshot.get("t", 0)) or 0)
                break

    if active_map is None:
        raise ReplayError("replay contains no player snapshots")
    if first_matching_ms is None:
        raise ReplayError(f"replay has no {event} event on active map {active_map}")
    return active_map, first_matching_ms


def paeth_predictor(left: int, up: int, upper_left: int) -> int:
    prediction = left + up - upper_left
    pa = abs(prediction - left)
    pb = abs(prediction - up)
    pc = abs(prediction - upper_left)
    if pa <= pb and pa <= pc:
        return left
    if pb <= pc:
        return up
    return upper_left


def unfilter_scanlines(raw: bytes, width: int, height: int, bytes_per_pixel: int) -> bytes:
    stride = width * bytes_per_pixel
    expected = height * (stride + 1)
    if len(raw) != expected:
        raise PngError(f"unexpected decompressed size: got {len(raw)}, expected {expected}")

    out = bytearray(height * stride)
    source_offset = 0
    dest_offset = 0
    previous_row = bytes(stride)

    for _row in range(height):
        filter_type = raw[source_offset]
        source_offset += 1
        row = bytearray(raw[source_offset:source_offset + stride])
        source_offset += stride

        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(stride):
                left = row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                row[i] = (row[i] + left) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                row[i] = (row[i] + previous_row[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                up = previous_row[i]
                row[i] = (row[i] + ((left + up) // 2)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                left = row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                up = previous_row[i]
                upper_left = previous_row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                row[i] = (row[i] + paeth_predictor(left, up, upper_left)) & 0xFF
        else:
            raise PngError(f"unsupported PNG row filter: {filter_type}")

        out[dest_offset:dest_offset + stride] = row
        dest_offset += stride
        previous_row = bytes(row)

    return bytes(out)


def parse_png(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        raise PngError("not a PNG file")

    offset = len(PNG_SIGNATURE)
    width = height = color_type = bit_depth = interlace = None
    idat = bytearray()

    while offset + 8 <= len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        chunk_type = data[offset + 4:offset + 8]
        offset += 8
        chunk_data = data[offset:offset + length]
        offset += length + 4  # skip CRC
        if len(chunk_data) != length:
            raise PngError("truncated PNG chunk")

        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _compression, _filter, interlace = struct.unpack(
                ">IIBBBBB", chunk_data
            )
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if width is None or height is None or color_type is None:
        raise PngError("missing PNG IHDR")
    if bit_depth != 8:
        raise PngError(f"unsupported PNG bit depth: {bit_depth}")
    if interlace != 0:
        raise PngError("interlaced PNGs are not supported by this smoke validator")

    bytes_per_pixel_by_color_type = {
        0: 1,  # grayscale
        2: 3,  # RGB
        4: 2,  # grayscale + alpha
        6: 4,  # RGBA
    }
    if color_type not in bytes_per_pixel_by_color_type:
        raise PngError(f"unsupported PNG color type: {color_type}")

    decompressed = zlib.decompress(bytes(idat))
    pixels = unfilter_scanlines(
        decompressed,
        width,
        height,
        bytes_per_pixel_by_color_type[color_type],
    )
    return width, height, color_type, pixels


def unique_color_count(pixels: bytes, color_type: int, minimum: int) -> int:
    if color_type == 0:
        step = 1
        color_size = 1
    elif color_type == 2:
        step = 3
        color_size = 3
    elif color_type == 4:
        step = 2
        color_size = 1
    elif color_type == 6:
        step = 4
        color_size = 3
    else:
        raise PngError(f"unsupported PNG color type: {color_type}")

    colors: set[bytes] = set()
    for offset in range(0, len(pixels), step):
        colors.add(pixels[offset:offset + color_size])
        if len(colors) >= minimum:
            return len(colors)
    return len(colors)


def validate_png(path: Path, min_width: int, min_height: int, min_unique_colors: int) -> None:
    if not path.exists():
        raise PngError(f"screenshot was not written: {path}")
    width, height, color_type, pixels = parse_png(path)
    if width < min_width or height < min_height:
        raise PngError(f"PNG too small: {width}x{height}, expected at least {min_width}x{min_height}")
    unique_colors = unique_color_count(pixels, color_type, min_unique_colors)
    if unique_colors < min_unique_colors:
        raise PngError(
            f"PNG appears blank: only {unique_colors} unique colors, expected at least {min_unique_colors}"
        )
    print(
        f"OK: {path} is {width}x{height}, color_type={color_type}, "
        f"unique_colors>={unique_colors}"
    )


def default_output_path(wowee: Path) -> Path:
    return wowee.resolve().parent / "wowee_replay_smoke.png"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path, nargs="?", help="Godview JSONL recording to replay.")
    parser.add_argument("--wowee", type=Path, default=Path("build/bin/wowee"))
    parser.add_argument("--data-path", type=Path, default=None, help="Extracted vanilla data directory.")
    parser.add_argument("--output", type=Path, default=None, help="Screenshot path to write.")
    parser.add_argument("--event", default="death", help="WOWEE_REPLAY_SCREENSHOT_EVENT value.")
    parser.add_argument("--focus", default=None, help="WOWEE_REPLAY_FOCUS_PLAYER value.")
    parser.add_argument("--no-follow", action="store_true", help="Do not enable replay follow camera.")
    parser.add_argument("--follow-distance", type=float, default=None, help="Replay follow camera distance.")
    parser.add_argument("--follow-height", type=float, default=None, help="Replay follow camera height.")
    parser.add_argument("--follow-steep-pitch", type=float, default=None, help="Steepest replay follow pitch in degrees.")
    parser.add_argument("--follow-shallow-pitch", type=float, default=None, help="Shallowest replay follow pitch in degrees.")
    parser.add_argument("--ms", type=float, default=None, help="Explicit WOWEE_REPLAY_SCREENSHOT_MS value.")
    parser.add_argument(
        "--event-offset-ms",
        type=float,
        default=None,
        help="Capture this many milliseconds after --event, keeping event focus/camera setup.",
    )
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--min-width", type=int, default=640)
    parser.add_argument("--min-height", type=int, default=360)
    parser.add_argument("--min-unique-colors", type=int, default=32)
    parser.add_argument("--validate-only", action="store_true", help="Validate --output without launching WoWee.")
    parser.add_argument("--no-clean-capture", action="store_true", help="Keep replay overlay/minimap chrome visible.")
    parser.add_argument(
        "--skip-event-preflight",
        action="store_true",
        help="Do not inspect the JSONL for the requested replay event before launching WoWee.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    wowee = args.wowee.resolve()
    replay = args.replay.resolve() if args.replay else None
    output = (args.output.resolve() if args.output else default_output_path(wowee))

    if args.validate_only:
        validate_png(output, args.min_width, args.min_height, args.min_unique_colors)
        return 0

    if not wowee.exists():
        print(f"error: WoWee binary not found: {wowee}", file=sys.stderr)
        return 2
    if replay is None:
        print("error: replay file is required unless --validate-only is set", file=sys.stderr)
        return 2
    if not replay.exists():
        print(f"error: replay file not found: {replay}", file=sys.stderr)
        return 2
    if args.ms is not None and args.event_offset_ms is not None:
        print("error: --ms and --event-offset-ms are mutually exclusive", file=sys.stderr)
        return 2

    data_path = args.data_path or (Path(os.environ["WOW_DATA_PATH"]) if "WOW_DATA_PATH" in os.environ else None)
    if data_path is None:
        print("error: pass --data-path or set WOW_DATA_PATH", file=sys.stderr)
        return 2

    event = normalize_event(args.event)
    if args.event_offset_ms is not None and event is None:
        print("error: --event-offset-ms requires an enabled --event", file=sys.stderr)
        return 2
    if event and args.ms is None and not args.skip_event_preflight:
        try:
            active_map, event_ms = preflight_replay_event(replay, event)
        except ReplayError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        if args.event_offset_ms is not None:
            print(
                f"OK: replay has {event} event on active map {active_map} at ms={event_ms}; "
                f"offset capture at ms={event_ms + args.event_offset_ms:g}"
            )
        else:
            print(f"OK: replay has {event} event on active map {active_map} at ms={event_ms}")

    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    env = os.environ.copy()
    env["WOW_DATA_PATH"] = str(data_path.resolve())
    env["WOWEE_REPLAY_SCREENSHOT_PATH"] = str(output)
    env["WOWEE_REPLAY_SCREENSHOT_EXIT"] = "1"
    env["WOWEE_REPLAY_SCREENSHOT_FRAMES"] = str(args.frames)
    if args.ms is not None:
        env["WOWEE_REPLAY_SCREENSHOT_MS"] = str(args.ms)
    elif event:
        env["WOWEE_REPLAY_SCREENSHOT_EVENT"] = event
        if args.event_offset_ms is not None:
            env["WOWEE_REPLAY_SCREENSHOT_EVENT_OFFSET_MS"] = str(args.event_offset_ms)
    focus = None
    if args.no_follow:
        env["WOWEE_REPLAY_FOLLOW_CAMERA"] = "0"
    else:
        env["WOWEE_REPLAY_FOLLOW_CAMERA"] = "1"
        focus = args.focus if args.focus is not None else (event if args.ms is None else "first")
    if focus:
        env["WOWEE_REPLAY_FOCUS_PLAYER"] = focus
    if args.follow_distance is not None:
        env["WOWEE_REPLAY_FOLLOW_DISTANCE"] = str(args.follow_distance)
    if args.follow_height is not None:
        env["WOWEE_REPLAY_FOLLOW_HEIGHT"] = str(args.follow_height)
    if args.follow_steep_pitch is not None:
        env["WOWEE_REPLAY_FOLLOW_STEEP_PITCH"] = str(args.follow_steep_pitch)
    if args.follow_shallow_pitch is not None:
        env["WOWEE_REPLAY_FOLLOW_SHALLOW_PITCH"] = str(args.follow_shallow_pitch)
    if not args.no_clean_capture:
        env["WOWEE_REPLAY_CLEAN_CAPTURE"] = "1"

    command = [str(wowee), "--replay", str(replay)]
    print("running:", " ".join(command))
    try:
        result = subprocess.run(
            command,
            cwd=wowee.parent,
            env=env,
            text=True,
            capture_output=True,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        if exc.stdout:
            stdout = exc.stdout.decode(errors="replace") if isinstance(exc.stdout, bytes) else exc.stdout
            print(stdout, end="")
        if exc.stderr:
            stderr = exc.stderr.decode(errors="replace") if isinstance(exc.stderr, bytes) else exc.stderr
            print(stderr, end="", file=sys.stderr)
        try:
            validate_png(output, args.min_width, args.min_height, args.min_unique_colors)
        except PngError as png_exc:
            print(f"error: WoWee timed out after {args.timeout:g}s and no valid screenshot was available: {png_exc}",
                  file=sys.stderr)
            return 124
        print(f"warning: WoWee timed out after {args.timeout:g}s after writing a valid screenshot; accepting capture",
              file=sys.stderr)
        return 0
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        print(f"error: WoWee exited with status {result.returncode}", file=sys.stderr)
        return result.returncode

    try:
        validate_png(output, args.min_width, args.min_height, args.min_unique_colors)
    except PngError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
