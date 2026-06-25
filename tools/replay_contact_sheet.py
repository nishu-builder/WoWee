#!/usr/bin/env python3
"""
Capture several replay screenshots around an event and assemble a labeled PNG sheet.

This is a convenience wrapper around replay_screenshot_smoke.py. Each frame is
still produced and validated through WoWee's real replay screenshot hook.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import zlib
from pathlib import Path

import replay_screenshot_smoke as smoke


FONT_3X5: dict[str, tuple[str, ...]] = {
    " ": ("000", "000", "000", "000", "000"),
    "+": ("000", "010", "111", "010", "000"),
    "-": ("000", "000", "111", "000", "000"),
    ":": ("000", "010", "000", "010", "000"),
    ".": ("000", "000", "000", "000", "010"),
    "0": ("111", "101", "101", "101", "111"),
    "1": ("010", "110", "010", "010", "111"),
    "2": ("111", "001", "111", "100", "111"),
    "3": ("111", "001", "111", "001", "111"),
    "4": ("101", "101", "111", "001", "001"),
    "5": ("111", "100", "111", "001", "111"),
    "6": ("111", "100", "111", "101", "111"),
    "7": ("111", "001", "010", "010", "010"),
    "8": ("111", "101", "111", "101", "111"),
    "9": ("111", "101", "111", "001", "111"),
    "A": ("010", "101", "111", "101", "101"),
    "B": ("110", "101", "110", "101", "110"),
    "C": ("111", "100", "100", "100", "111"),
    "D": ("110", "101", "101", "101", "110"),
    "E": ("111", "100", "110", "100", "111"),
    "F": ("111", "100", "110", "100", "100"),
    "G": ("111", "100", "101", "101", "111"),
    "H": ("101", "101", "111", "101", "101"),
    "I": ("111", "010", "010", "010", "111"),
    "J": ("001", "001", "001", "101", "111"),
    "K": ("101", "101", "110", "101", "101"),
    "L": ("100", "100", "100", "100", "111"),
    "M": ("101", "111", "111", "101", "101"),
    "N": ("101", "111", "111", "111", "101"),
    "O": ("111", "101", "101", "101", "111"),
    "P": ("111", "101", "111", "100", "100"),
    "Q": ("111", "101", "101", "111", "001"),
    "R": ("111", "101", "111", "110", "101"),
    "S": ("111", "100", "111", "001", "111"),
    "T": ("111", "010", "010", "010", "010"),
    "U": ("101", "101", "101", "101", "111"),
    "V": ("101", "101", "101", "101", "010"),
    "W": ("101", "101", "111", "111", "101"),
    "X": ("101", "101", "010", "101", "101"),
    "Y": ("101", "101", "010", "010", "010"),
    "Z": ("111", "001", "010", "100", "111"),
}


def parse_offsets(value: str) -> list[float]:
    offsets: list[float] = []
    for raw in value.split(","):
        raw = raw.strip()
        if not raw:
            continue
        offset = float(raw)
        if offset < 0:
            raise argparse.ArgumentTypeError("offsets must be non-negative")
        offsets.append(offset)
    if not offsets:
        raise argparse.ArgumentTypeError("at least one offset is required")
    return offsets


def parse_ms_values(value: str) -> list[float]:
    values: list[float] = []
    for raw in value.split(","):
        raw = raw.strip()
        if not raw:
            continue
        ms = float(raw)
        if ms < 0:
            raise argparse.ArgumentTypeError("server ms values must be non-negative")
        values.append(ms)
    if not values:
        raise argparse.ArgumentTypeError("at least one server ms value is required")
    return values


def format_offset(offset_ms: float) -> str:
    if offset_ms >= 1000 and math.isclose(offset_ms % 1000, 0.0, abs_tol=0.001):
        seconds = int(offset_ms / 1000)
        return f"+{seconds}S"
    if math.isclose(offset_ms, round(offset_ms), abs_tol=0.001):
        return f"+{int(round(offset_ms))}MS"
    return f"+{offset_ms:g}MS"


def format_ms(ms: float) -> str:
    if math.isclose(ms, round(ms), abs_tol=0.001):
        return str(int(round(ms)))
    return f"{ms:g}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path, help="Godview JSONL recording to replay.")
    parser.add_argument("--wowee", type=Path, default=Path("build/bin/wowee"))
    parser.add_argument("--data-path", type=Path, required=True, help="Extracted vanilla data directory.")
    parser.add_argument("--event", action="append", default=None, help="Replay event to capture; repeatable.")
    parser.add_argument("--offset-ms", type=parse_offsets, default=parse_offsets("0,500,3000"))
    parser.add_argument(
        "--ms",
        type=parse_ms_values,
        default=None,
        help="Comma-separated explicit server ms values to capture instead of event offsets.",
    )
    parser.add_argument(
        "--timeline-samples",
        type=int,
        default=None,
        help="Capture this many evenly spaced samples across the active replay map timeline.",
    )
    parser.add_argument("--output", type=Path, required=True, help="Contact sheet PNG path.")
    parser.add_argument("--frames-dir", type=Path, default=None, help="Directory for per-frame PNGs.")
    parser.add_argument("--columns", type=int, default=3)
    parser.add_argument("--thumb-width", type=int, default=426)
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--min-frame-delta",
        type=float,
        default=0.0,
        help=(
            "Require this minimum average RGB byte difference between each "
            "adjacent pair of captured thumbnails; useful for catching stale "
            "multi-frame captures. Disabled by default."
        ),
    )
    parser.add_argument("--keep-overlay", action="store_true", help="Keep replay overlay/minimap chrome visible.")
    parser.add_argument("--no-follow", action="store_true", help="Do not enable replay follow camera.")
    parser.add_argument("--follow-distance", type=float, default=None, help="Replay follow camera distance.")
    parser.add_argument("--follow-height", type=float, default=None, help="Replay follow camera height.")
    parser.add_argument("--follow-steep-pitch", type=float, default=None, help="Steepest replay follow pitch in degrees.")
    parser.add_argument("--follow-shallow-pitch", type=float, default=None, help="Shallowest replay follow pitch in degrees.")
    parser.add_argument(
        "--require-event-cue-colors",
        action="store_true",
        help="Require semantic damage/death cue colors in event-offset captures while their cues should be active.",
    )
    parser.add_argument("--damage-cue-window-ms", type=float, default=1200.0)
    parser.add_argument("--death-cue-window-ms", type=float, default=3500.0)
    parser.add_argument(
        "--min-cue-color-pixels",
        type=int,
        default=None,
        help="Override the per-event cue color pixel thresholds.",
    )
    parser.add_argument("--min-damage-cue-color-pixels", type=int, default=80)
    parser.add_argument("--min-death-cue-color-pixels", type=int, default=10)
    return parser.parse_args()


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    import struct

    crc = zlib.crc32(kind)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)


def write_png_rgba(path: Path, width: int, height: int, pixels: bytes) -> None:
    import struct

    if len(pixels) != width * height * 4:
        raise ValueError("RGBA buffer size does not match dimensions")
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        start = y * stride
        raw.extend(pixels[start:start + stride])

    data = bytearray(smoke.PNG_SIGNATURE)
    data.extend(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
    data.extend(png_chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
    data.extend(png_chunk(b"IEND", b""))
    path.write_bytes(bytes(data))


def read_rgba(path: Path) -> tuple[int, int, bytes]:
    width, height, color_type, pixels = smoke.parse_png(path)
    if color_type == 6:
        return width, height, pixels
    if color_type == 2:
        rgba = bytearray(width * height * 4)
        out = 0
        for i in range(0, len(pixels), 3):
            rgba[out:out + 3] = pixels[i:i + 3]
            rgba[out + 3] = 255
            out += 4
        return width, height, bytes(rgba)
    if color_type == 0:
        rgba = bytearray(width * height * 4)
        out = 0
        for value in pixels:
            rgba[out:out + 4] = bytes((value, value, value, 255))
            out += 4
        return width, height, bytes(rgba)
    if color_type == 4:
        rgba = bytearray(width * height * 4)
        out = 0
        for i in range(0, len(pixels), 2):
            value = pixels[i]
            rgba[out:out + 4] = bytes((value, value, value, pixels[i + 1]))
            out += 4
        return width, height, bytes(rgba)
    raise smoke.PngError(f"unsupported PNG color type for sheet: {color_type}")


def resize_rgba_nearest(width: int, height: int, pixels: bytes, target_width: int) -> tuple[int, int, bytes]:
    target_width = min(width, max(1, target_width))
    target_height = max(1, round(height * (target_width / width)))
    out = bytearray(target_width * target_height * 4)
    for y in range(target_height):
        source_y = min(height - 1, int(y * height / target_height))
        for x in range(target_width):
            source_x = min(width - 1, int(x * width / target_width))
            src = (source_y * width + source_x) * 4
            dst = (y * target_width + x) * 4
            out[dst:dst + 4] = pixels[src:src + 4]
    return target_width, target_height, bytes(out)


def paste(dest: bytearray, dest_width: int, x0: int, y0: int, src_width: int, src_height: int, src: bytes) -> None:
    for y in range(src_height):
        dst_offset = ((y0 + y) * dest_width + x0) * 4
        src_offset = y * src_width * 4
        dest[dst_offset:dst_offset + src_width * 4] = src[src_offset:src_offset + src_width * 4]


def fill_rect(pixels: bytearray, width: int, x0: int, y0: int, w: int, h: int, color: tuple[int, int, int, int]) -> None:
    r, g, b, a = color
    row = bytes((r, g, b, a)) * max(0, w)
    for y in range(max(0, h)):
        offset = ((y0 + y) * width + x0) * 4
        pixels[offset:offset + len(row)] = row


def draw_text(
    pixels: bytearray,
    width: int,
    x0: int,
    y0: int,
    text: str,
    scale: int = 2,
    color: tuple[int, int, int, int] = (235, 240, 245, 255),
) -> None:
    cursor = x0
    for ch in text.upper():
        glyph = FONT_3X5.get(ch, FONT_3X5[" "])
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit != "1":
                    continue
                fill_rect(pixels, width, cursor + gx * scale, y0 + gy * scale, scale, scale, color)
        cursor += (3 + 1) * scale


def average_rgb_delta(
    a_width: int,
    a_height: int,
    a_pixels: bytes,
    b_width: int,
    b_height: int,
    b_pixels: bytes,
) -> float:
    width = min(a_width, b_width)
    height = min(a_height, b_height)
    if width <= 0 or height <= 0:
        return 0.0

    total = 0
    channels = width * height * 3
    for y in range(height):
        a_row = y * a_width * 4
        b_row = y * b_width * 4
        for x in range(width):
            ai = a_row + x * 4
            bi = b_row + x * 4
            total += abs(a_pixels[ai] - b_pixels[bi])
            total += abs(a_pixels[ai + 1] - b_pixels[bi + 1])
            total += abs(a_pixels[ai + 2] - b_pixels[bi + 2])
    return total / float(channels)


def frame_name(event: str, offset_ms: float) -> str:
    clean = event.replace("/", "-").replace(" ", "-")
    return f"{clean}_{int(round(offset_ms))}ms.png"


def ms_frame_name(ms: float) -> str:
    return f"ms_{format_ms(ms)}.png"


def timeline_frame_name(index: int, ms: float) -> str:
    return f"timeline_{index:02d}_ms_{format_ms(ms)}.png"


def format_capture_label(event: str, offset_ms: float, active_map: int, capture_ms: float) -> str:
    return f"{event} {format_offset(offset_ms)} M{active_map} MS{format_ms(capture_ms)}"


def append_camera_args(command: list[str], args: argparse.Namespace) -> None:
    if args.no_follow:
        command.append("--no-follow")
    if args.follow_distance is not None:
        command.extend(["--follow-distance", str(args.follow_distance)])
    if args.follow_height is not None:
        command.extend(["--follow-height", str(args.follow_height)])
    if args.follow_steep_pitch is not None:
        command.extend(["--follow-steep-pitch", str(args.follow_steep_pitch)])
    if args.follow_shallow_pitch is not None:
        command.extend(["--follow-shallow-pitch", str(args.follow_shallow_pitch)])


def expected_cue_color(args: argparse.Namespace, event: str, offset_ms: float) -> str | None:
    if not args.require_event_cue_colors:
        return None
    if event == "damage" and offset_ms <= args.damage_cue_window_ms:
        return "damage"
    if event == "death" and offset_ms <= args.death_cue_window_ms:
        return "death"
    return None


def min_cue_color_pixels(args: argparse.Namespace, event: str) -> int:
    if args.min_cue_color_pixels is not None:
        return args.min_cue_color_pixels
    if event == "damage":
        return args.min_damage_cue_color_pixels
    if event == "death":
        return args.min_death_cue_color_pixels
    return 10


def active_map_timeline(path: Path, samples: int) -> tuple[int, list[float]]:
    if samples <= 0:
        raise smoke.ReplayError("--timeline-samples must be positive")

    active_map: int | None = None
    first_ms: int | None = None
    last_ms: int | None = None

    with path.open("r", encoding="utf-8") as file:
        for line_number, line in enumerate(file, start=1):
            if not line.strip():
                continue
            try:
                snapshot = json.loads(line)
            except json.JSONDecodeError as exc:
                raise smoke.ReplayError(f"JSON parse error at line {line_number}: {exc}") from exc
            if not isinstance(snapshot, dict):
                raise smoke.ReplayError(f"line {line_number} is not a JSON object")

            players = snapshot.get("players")
            if not isinstance(players, list) or not players:
                continue

            snapshot_map = int(snapshot.get("map", 0) or 0)
            if active_map is None:
                active_map = snapshot_map
            if snapshot_map != active_map:
                continue

            snapshot_ms = int(snapshot.get("ms", snapshot.get("t", 0)) or 0)
            if first_ms is None:
                first_ms = snapshot_ms
            last_ms = snapshot_ms

    if active_map is None or first_ms is None or last_ms is None:
        raise smoke.ReplayError("replay contains no player snapshots")
    if samples == 1 or first_ms == last_ms:
        return active_map, [float(first_ms)]

    span = float(last_ms - first_ms)
    return active_map, [
        float(first_ms) + span * (index / float(samples - 1))
        for index in range(samples)
    ]


def run_smoke(args: argparse.Namespace, event: str, offset_ms: float, output: Path) -> None:
    script = Path(__file__).with_name("replay_screenshot_smoke.py")
    command = [
        sys.executable,
        str(script),
        str(args.replay),
        "--wowee",
        str(args.wowee),
        "--data-path",
        str(args.data_path),
        "--event",
        event,
        "--event-offset-ms",
        str(offset_ms),
        "--output",
        str(output),
        "--frames",
        str(args.frames),
        "--timeout",
        str(args.timeout),
    ]
    if args.keep_overlay:
        command.append("--no-clean-capture")
    append_camera_args(command, args)
    cue_color = expected_cue_color(args, event, offset_ms)
    if cue_color:
        command.extend(["--expect-cue-color", cue_color])
        command.extend(["--min-cue-color-pixels", str(min_cue_color_pixels(args, cue_color))])

    print("running:", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def run_ms_smoke(args: argparse.Namespace, ms: float, output: Path) -> None:
    script = Path(__file__).with_name("replay_screenshot_smoke.py")
    command = [
        sys.executable,
        str(script),
        str(args.replay),
        "--wowee",
        str(args.wowee),
        "--data-path",
        str(args.data_path),
        "--ms",
        str(ms),
        "--output",
        str(output),
        "--frames",
        str(args.frames),
        "--timeout",
        str(args.timeout),
    ]
    if args.keep_overlay:
        command.append("--no-clean-capture")
    append_camera_args(command, args)

    print("running:", " ".join(command), flush=True)
    subprocess.run(command, check=True)


def validate_frame_deltas(
    thumbnails: list[tuple[str, int, int, bytes]],
    min_frame_delta: float,
) -> None:
    if min_frame_delta <= 0.0 or len(thumbnails) < 2:
        return

    for index in range(1, len(thumbnails)):
        prev_label, prev_width, prev_height, prev_pixels = thumbnails[index - 1]
        label, width, height, pixels = thumbnails[index]
        delta = average_rgb_delta(prev_width, prev_height, prev_pixels, width, height, pixels)
        if delta < min_frame_delta:
            raise smoke.PngError(
                f"adjacent frames look stale: {prev_label!r} -> {label!r} "
                f"average RGB delta {delta:.3f} < {min_frame_delta:.3f}"
            )
        print(
            f"OK: frame delta {prev_label!r} -> {label!r} "
            f"average RGB delta {delta:.3f} >= {min_frame_delta:.3f}"
        )


def assemble_sheet(
    frames: list[tuple[str, Path]],
    output: Path,
    columns: int,
    thumb_width: int,
    min_frame_delta: float,
) -> None:
    if columns <= 0:
        raise ValueError("--columns must be positive")
    thumbnails: list[tuple[str, int, int, bytes]] = []
    for label, path in frames:
        width, height, pixels = read_rgba(path)
        thumb_w, thumb_h, thumb_pixels = resize_rgba_nearest(width, height, pixels, thumb_width)
        thumbnails.append((label, thumb_w, thumb_h, thumb_pixels))
    validate_frame_deltas(thumbnails, min_frame_delta)

    label_height = 24
    padding = 10
    rows = math.ceil(len(thumbnails) / columns)
    cell_width = max(width for _label, width, _height, _pixels in thumbnails)
    cell_height = max(height for _label, _width, height, _pixels in thumbnails) + label_height
    sheet_width = columns * cell_width + (columns + 1) * padding
    sheet_height = rows * cell_height + (rows + 1) * padding
    sheet = bytearray(bytes((18, 20, 23, 255)) * sheet_width * sheet_height)

    for index, (label, thumb_w, thumb_h, thumb_pixels) in enumerate(thumbnails):
        col = index % columns
        row = index // columns
        x = padding + col * (cell_width + padding)
        y = padding + row * (cell_height + padding)
        fill_rect(sheet, sheet_width, x, y, cell_width, label_height, (38, 43, 50, 255))
        draw_text(sheet, sheet_width, x + 8, y + 6, label)
        paste(sheet, sheet_width, x, y + label_height, thumb_w, thumb_h, thumb_pixels)

    output.parent.mkdir(parents=True, exist_ok=True)
    write_png_rgba(output, sheet_width, sheet_height, bytes(sheet))
    smoke.validate_png(output, 320, 180, 32)


def main() -> int:
    args = parse_args()
    explicit_modes = sum(
        1
        for enabled in (args.ms is not None, bool(args.event), args.timeline_samples is not None)
        if enabled
    )
    if explicit_modes > 1:
        print("error: choose only one of --event, --ms, or --timeline-samples", file=sys.stderr)
        return 2

    events = args.event or ["damage"]
    frames_dir = args.frames_dir or args.output.with_suffix("").parent / f"{args.output.stem}_frames"
    frames_dir.mkdir(parents=True, exist_ok=True)

    frames: list[tuple[str, Path]] = []
    if args.ms is not None:
        for ms in args.ms:
            output = frames_dir / ms_frame_name(ms)
            run_ms_smoke(args, ms, output)
            frames.append((f"MS{format_ms(ms)}", output))
        assemble_sheet(frames, args.output, args.columns, args.thumb_width, args.min_frame_delta)
        print(f"OK: wrote replay contact sheet {args.output}")
        return 0

    if args.timeline_samples is not None:
        try:
            active_map, timeline = active_map_timeline(args.replay, args.timeline_samples)
        except smoke.ReplayError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        for index, ms in enumerate(timeline, start=1):
            output = frames_dir / timeline_frame_name(index, ms)
            run_ms_smoke(args, ms, output)
            frames.append((f"T{index} M{active_map} MS{format_ms(ms)}", output))
        assemble_sheet(frames, args.output, args.columns, args.thumb_width, args.min_frame_delta)
        print(f"OK: wrote replay contact sheet {args.output}")
        return 0

    for event in events:
        normalized_event = smoke.normalize_event(event)
        if not normalized_event:
            print(f"error: unsupported or disabled event: {event}", file=sys.stderr)
            return 2
        try:
            active_map, event_ms = smoke.preflight_replay_event(args.replay, normalized_event)
        except smoke.ReplayError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 1
        for offset_ms in args.offset_ms:
            output = frames_dir / frame_name(normalized_event, offset_ms)
            run_smoke(args, normalized_event, offset_ms, output)
            label = format_capture_label(normalized_event, offset_ms, active_map, event_ms + offset_ms)
            frames.append((label, output))

    assemble_sheet(frames, args.output, args.columns, args.thumb_width, args.min_frame_delta)
    print(f"OK: wrote replay contact sheet {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
