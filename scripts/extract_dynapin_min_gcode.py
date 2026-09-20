#!/usr/bin/env python3
"""Create compact G-code files containing DynaPin pulls and nearby layers."""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, Sequence


_LAYER_CHANGE_RE = re.compile(rb"^\s*;\s*LAYER_CHANGE(?:\s|$)")
_BEGIN_PULL_RE = re.compile(rb"^\s*;\s*BEGIN_DYNAPIN_PULL(?:\s|$)")
_END_PULL_RE = re.compile(rb"^\s*;\s*END_DYNAPIN_PULL(?:\s|$)")


@dataclass(frozen=True)
class Layer:
    start: int
    end: int
    pull_spans: tuple[tuple[int, int], ...]


class ExtractionError(ValueError):
    """Raised when a source G-code cannot satisfy the extraction contract."""


def _is_layer_change(line: bytes) -> bool:
    return _LAYER_CHANGE_RE.match(line) is not None


def _is_begin_pull(line: bytes) -> bool:
    return _BEGIN_PULL_RE.match(line) is not None


def _is_end_pull(line: bytes) -> bool:
    return _END_PULL_RE.match(line) is not None


def _parse_layers(lines: Sequence[bytes]) -> Optional[list[Layer]]:
    layer_starts = [index for index, line in enumerate(lines) if _is_layer_change(line)]
    has_pull_marker = any(_is_begin_pull(line) or _is_end_pull(line) for line in lines)

    if not has_pull_marker:
        return None
    if not layer_starts:
        raise ExtractionError("DynaPin markers were found before any ;LAYER_CHANGE")

    pull_spans_by_layer: list[list[tuple[int, int]]] = [[] for _ in layer_starts]
    current_layer: Optional[int] = None
    begin_index: Optional[int] = None
    begin_layer: Optional[int] = None

    layer_index_by_start = {start: index for index, start in enumerate(layer_starts)}
    for line_index, line in enumerate(lines):
        if line_index in layer_index_by_start:
            current_layer = layer_index_by_start[line_index]

        if _is_begin_pull(line):
            if begin_index is not None:
                raise ExtractionError(f"nested BEGIN_DYNAPIN_PULL at source line {line_index + 1}")
            if current_layer is None:
                raise ExtractionError(f"BEGIN_DYNAPIN_PULL before a layer at source line {line_index + 1}")
            begin_index = line_index
            begin_layer = current_layer
            continue

        if _is_end_pull(line):
            if begin_index is None or begin_layer is None:
                raise ExtractionError(f"END_DYNAPIN_PULL without BEGIN at source line {line_index + 1}")
            if current_layer != begin_layer:
                raise ExtractionError(
                    "DynaPin pull block crosses a ;LAYER_CHANGE "
                    f"(started at source line {begin_index + 1}, ended at source line {line_index + 1})"
                )
            pull_spans_by_layer[begin_layer].append((begin_index, line_index + 1))
            begin_index = None
            begin_layer = None

    if begin_index is not None:
        raise ExtractionError(f"BEGIN_DYNAPIN_PULL at source line {begin_index + 1} has no END_DYNAPIN_PULL")

    layer_ends = layer_starts[1:] + [len(lines)]
    return [
        Layer(start=start, end=end, pull_spans=tuple(pull_spans))
        for start, end, pull_spans in zip(layer_starts, layer_ends, pull_spans_by_layer)
    ]


def _merge_spans(spans: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for start, end in sorted(spans):
        if not merged or start > merged[-1][1]:
            merged.append((start, end))
        else:
            previous_start, previous_end = merged[-1]
            merged[-1] = (previous_start, max(previous_end, end))
    return merged


def extract_min_payload(base_content: bytes, source_content: bytes) -> Optional[bytes]:
    """Return base plus selected source spans, or None when no pull markers exist."""

    lines = source_content.splitlines(keepends=True)
    layers = _parse_layers(lines)
    if layers is None:
        return None

    selected_spans: list[tuple[int, int]] = []
    for layer_index, layer in enumerate(layers):
        if not layer.pull_spans:
            continue
        if layer_index + 4 >= len(layers):
            raise ExtractionError(
                f"DynaPin layer at source line {layer.start + 1} has fewer than four following layers"
            )

        selected_spans.extend(layer.pull_spans)
        selected_spans.extend(
            (layers[next_layer].start, layers[next_layer].end)
            for next_layer in range(layer_index + 1, layer_index + 5)
        )

    selected_spans = _merge_spans(selected_spans)
    selected_content = b"".join(
        b"".join(lines[start:end])
        for start, end in selected_spans
    )
    separator = b"" if not base_content or base_content.endswith((b"\r", b"\n")) else b"\n"
    return base_content + separator + selected_content


def discover_gcodes(outputs_dir: Path) -> list[Path]:
    """Return sorted recursive source G-code paths, excluding min_*.gcode."""

    if not outputs_dir.is_dir():
        return []
    return sorted(
        (
            path
            for path in outputs_dir.rglob("*")
            if path.is_file()
            and path.suffix.casefold() == ".gcode"
            and not path.name.casefold().startswith("min_")
        ),
        key=lambda path: path.as_posix().casefold(),
    )


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
    repository_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Extract DynaPin pull blocks and four following layers from output G-code files."
    )
    parser.add_argument(
        "--models-dir",
        type=Path,
        default=repository_root / "models",
        help="models directory containing min/min.gcode; outputs/ is its sibling (default: repository models/)",
    )
    parser.add_argument(
        "--base-file",
        type=Path,
        default=None,
        help="base G-code to copy before each extraction (default: <models-dir>/min/min.gcode)",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    models_dir = args.models_dir.expanduser()
    base_path = (args.base_file or models_dir / "min" / "min.gcode").expanduser()
    outputs_dir = models_dir.parent / "outputs"

    if not base_path.is_file():
        print(f"error: base G-code does not exist: {base_path}", file=sys.stderr)
        return 2
    if not outputs_dir.is_dir():
        print(f"error: outputs directory does not exist: {outputs_dir}", file=sys.stderr)
        return 2

    try:
        base_content = base_path.read_bytes()
    except OSError as error:
        print(f"error: cannot read base G-code {base_path}: {error}", file=sys.stderr)
        return 2

    sources = discover_gcodes(outputs_dir)
    if not sources:
        print(f"error: no source G-code files found under {outputs_dir}", file=sys.stderr)
        return 2

    created = 0
    skipped = 0
    failures = 0
    for source_path in sources:
        try:
            source_content = source_path.read_bytes()
            payload = extract_min_payload(base_content, source_content)
            if payload is None:
                print(f"skipped (no DynaPin pull markers): {source_path}")
                skipped += 1
                continue

            output_path = source_path.with_name(f"min_{source_path.stem}.gcode")
            _write_atomically(output_path, payload)
            print(f"created: {output_path}")
            created += 1
        except (ExtractionError, OSError) as error:
            print(f"error: {source_path}: {error}", file=sys.stderr)
            failures += 1

    print(f"summary: created={created}, skipped={skipped}, errors={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
