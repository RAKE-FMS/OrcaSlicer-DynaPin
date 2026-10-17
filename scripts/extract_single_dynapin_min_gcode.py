#!/usr/bin/env python3
"""Extract DynaPin pull blocks and four following layers from one G-code file."""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path
from typing import Optional, Sequence

from extract_dynapin_min_gcode import ExtractionError, extract_min_payload


DEFAULT_BASE_FILE = Path(__file__).resolve().parents[1] / "old-models/min/min.gcode"


def _write_atomically(output_path: Path, content: bytes) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f".{output_path.name}.{os.getpid()}.tmp")
    try:
        temporary_path.write_bytes(content)
        os.replace(temporary_path, output_path)
    finally:
        try:
            temporary_path.unlink()
        except FileNotFoundError:
            pass


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="source G-code containing DynaPin pull markers")
    parser.add_argument(
        "--base-file",
        type=Path,
        default=DEFAULT_BASE_FILE,
        help=f"base G-code copied before the extracted moves (default: {DEFAULT_BASE_FILE})",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="output path (default: min_<source-name>.gcode beside the source)",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    source_path = args.source.expanduser()
    base_path = args.base_file.expanduser()
    output_path = args.output.expanduser() if args.output else source_path.with_name(f"min_{source_path.stem}.gcode")

    try:
        if not source_path.is_file():
            raise OSError(f"source G-code does not exist: {source_path}")
        if not base_path.is_file():
            raise OSError(f"base G-code does not exist: {base_path}")
        if output_path.resolve() in (source_path.resolve(), base_path.resolve()):
            raise ValueError("output must be different from both source and base G-code files")

        payload = extract_min_payload(base_path.read_bytes(), source_path.read_bytes())
        if payload is None:
            raise ExtractionError(f"no DynaPin pull markers found in {source_path}")
        _write_atomically(output_path, payload)
    except (ExtractionError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"created: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
