#!/usr/bin/env python3
"""Copy min.gcode, then append DynaPin pull motion (no printer I/O)."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Sequence


DEFAULT_CONFIG = Path(__file__).resolve().parents[1] / "resources/profiles/Kingroon/dynapin/kp3s.json"
DEFAULT_BASE_FILE = Path(__file__).resolve().parents[1] / "old-models/min/min.gcode"
PULL_TYPES = ("normalonly", "revertedonly", "alternating")
MOVE_FIELDS = (
    "x_hook", "x_latch", "x_front", "y_offset", "approach_y_offset",
    "approach_z_offset", "engage_z_drop", "disengage_x_offset",
    "disengage_z_lift", "retreat_z_lift", "pull_feedrate_fast",
    "feed_rate", "fast_feed_rate",
)


def number(value: object) -> float:
    if isinstance(value, bool):
        raise ValueError("boolean values cannot be used as coordinates")
    result = float(value)
    if not math.isfinite(result):
        raise ValueError("coordinates and feed rates must be finite")
    return result


def load_config(path: Path) -> dict:
    config = json.loads(path.read_text(encoding="utf-8"))
    grid = config["grid"]
    for key in ("row_count", "col_count"):
        value = number(grid[key])
        if not value.is_integer() or value <= 0:
            raise ValueError(f"grid.{key} must be a positive integer")
        grid[key] = int(value)
    for key in ("y", "z"):
        grid["pull_origin"][key] = number(grid["pull_origin"][key])
    for key in ("row_z", "col_y"):
        grid["pitch"][key] = number(grid["pitch"][key])
    pull = config["pull_gcode"]
    if pull["type"] not in PULL_TYPES:
        raise ValueError(f"pull_gcode.type must be one of {PULL_TYPES}")
    parity = pull["reverted"]["row_parity"]
    if isinstance(parity, bool) or not isinstance(parity, int) or parity not in (0, 1):
        raise ValueError("reverted.row_parity must be 0 or 1")
    for kind in ("normal", "reverted"):
        for key in ("y_origin_offset", "alternating_y_offset", *MOVE_FIELDS):
            pull[kind][key] = number(pull[kind][key])
        for key in ("pull_feedrate_fast", "feed_rate", "fast_feed_rate"):
            if pull[kind][key] <= 0:
                raise ValueError(f"{kind}.{key} must be positive")
    return config


def parse_pins(value: str) -> list[tuple[int, int]]:
    pins = []
    for token in re.split(r"[;\s]+", value.strip()):
        parts = token.split(",")
        if len(parts) != 2:
            raise ValueError(f"invalid pin {token!r}; use row,col separated by spaces or semicolons")
        pins.append((int(parts[0]), int(parts[1])))
    return pins


def pull_parameters(config: dict, pin: tuple[int, int], mode: str) -> dict:
    pull = config["pull_gcode"]
    reverted = mode == "revertedonly" or (mode == "alternating" and pin[0] % 2 == pull["reverted"]["row_parity"])
    return pull["reverted" if reverted else "normal"]


def pull_gcode_for_pin(config: dict, pin: tuple[int, int], mode: str) -> str:
    """Current matches C++; revertedonly swaps Y positions; alternating engages in Z."""
    row, col = pin
    grid = config["grid"]
    if not 0 <= row < grid["row_count"] or not 0 <= col < grid["col_count"]:
        raise ValueError(f"pin {row},{col} is outside the configured grid")
    if mode not in ("current", *PULL_TYPES):
        raise ValueError(f"unknown pull type {mode!r}")
    params = pull_parameters(config, pin, mode)
    y = grid["pull_origin"]["y"] + col * grid["pitch"]["col_y"] + params["y_offset"]
    z = grid["pull_origin"]["z"] + row * grid["pitch"]["row_z"]
    if mode != "current":
        y += params["y_origin_offset"]
    if mode == "alternating":
        y += params["alternating_y_offset"]
    y_approach = y + params["approach_y_offset"]
    if mode == "revertedonly":
        y, y_approach = y_approach, y
    fast = params["fast_feed_rate"]
    slow = params["feed_rate"]
    lines = [f"; BEGIN_DYNAPIN_PULL ROW={row} COL={col}"]

    def move(feed: float, **axes: float) -> None:
        words = [f"{axis.upper()}{value:.4f}" for axis, value in axes.items()]
        lines.append(f"G1 {' '.join(words)} F{feed:.4f}")

    move(fast, y=y_approach)
    if mode == "alternating":
        z_approach = z + params["approach_z_offset"]
        z_engage = z_approach - params["engage_z_drop"]
        move(fast, x=params["x_hook"], z=z_approach)
        move(slow, x=params["x_latch"])
        move(slow, z=z_engage)
        lines.append("; DYNAPIN_PULL_MOVE")
        move(params["pull_feedrate_fast"], x=params["x_front"])
        move(slow, x=params["x_front"] + params["disengage_x_offset"],
             z=z_engage + params["disengage_z_lift"])
    else:
        move(fast, x=params["x_hook"], z=z)
        move(slow, x=params["x_latch"])
        move(slow, y=y)
        lines.append("; DYNAPIN_PULL_MOVE")
        move(params["pull_feedrate_fast"], x=params["x_front"])
        move(slow, x=params["x_front"] + params["disengage_x_offset"], y=y_approach)
        move(slow, z=z + params["retreat_z_lift"])
    lines.append("; END_DYNAPIN_PULL")
    return "\n".join(lines) + "\n"


def generate_gcode(config: dict, pins: Sequence[tuple[int, int]], mode: str | None = None,
                   return_position: Sequence[float] | None = None) -> str:
    mode = config["pull_gcode"]["type"] if mode is None else mode
    # Match C++: increasing support-top Z, then row and col, with duplicate pins removed.
    ordered = sorted(set(pins), key=lambda pin: (pin[0] * config["grid"]["pitch"]["row_z"], pin))
    if not ordered:
        raise ValueError("at least one pin is required")
    blocks = [pull_gcode_for_pin(config, pin, mode) for pin in ordered]
    result = f"; DynaPin Python pull verification TYPE={mode}\nG21\nG90\n" + "".join(blocks)
    if return_position is not None:
        x, y, z = (number(value) for value in return_position)
        params = pull_parameters(config, ordered[-1], mode)
        result += (
            "; BEGIN_DYNAPIN_RETURN\n"
            f"G1 X{x:.4f} Y{y:.4f} F{params['fast_feed_rate']:.4f}\n"
            f"G1 Z{z:.4f} F{params['feed_rate']:.4f}\n"
            "; END_DYNAPIN_RETURN\n"
        )
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG)
    parser.add_argument("--base-file", type=Path, default=DEFAULT_BASE_FILE,
                        help="G-code to copy before pull motion (default: repository old-models/min/min.gcode)")
    parser.add_argument("--pins", required=True, help='zero-based pins, e.g. "0,0; 1,0"')
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--type", choices=PULL_TYPES, help="override pull_gcode.type")
    modes.add_argument("--current", action="store_true", help="reproduce current C++ per-pin motion without orientation offsets")
    parser.add_argument("--return-position", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--output", type=Path, help="write a file instead of stdout")
    args = parser.parse_args(argv)
    try:
        base_path = args.base_file.expanduser()
        base_content = base_path.read_bytes()
        if args.output is not None and args.output.exists() and args.output.samefile(base_path):
            raise ValueError("output must be different from the base G-code file")
        config = load_config(args.config)
        gcode = generate_gcode(config, parse_pins(args.pins), "current" if args.current else args.type,
                               args.return_position)
        separator = b"" if not base_content or base_content.endswith((b"\n", b"\r")) else b"\n"
        content = base_content + separator + gcode.encode("utf-8")
        if args.output is None:
            sys.stdout.buffer.write(content)
        else:
            args.output.write_bytes(content)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
