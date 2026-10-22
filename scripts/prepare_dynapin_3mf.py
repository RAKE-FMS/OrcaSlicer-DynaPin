#!/usr/bin/env python3
"""Slice one DynaPin 3MF and prepare its full, minimal, and pull G-code files."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence
from zipfile import BadZipFile, ZipFile

from extract_dynapin_min_gcode import ExtractionError, extract_min_payload
from slice_models_by_filament import resolve_slicer, slice_one


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASE_FILE = REPOSITORY_ROOT / "old-models/min/min.gcode"
DEFAULT_CONFIG = REPOSITORY_ROOT / "resources/profiles/Kingroon/dynapin/kp3s.json"
PULL_GENERATOR = Path(__file__).with_name("generate_dynapin_pull_gcode.py")
PULL_BEGIN_RE = re.compile(
    rb"^\s*;\s*BEGIN_DYNAPIN_PULL\s+ROW=(\d+)\s+COL=(\d+)(?:\s|$)",
    re.MULTILINE,
)


def _orca_data_dir() -> Path:
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/OrcaSlicer"
    if os.name == "nt":
        appdata = os.environ.get("APPDATA")
        return Path(appdata) / "OrcaSlicer" if appdata else Path.home() / "AppData/Roaming/OrcaSlicer"
    config_home = os.environ.get("XDG_CONFIG_HOME")
    return Path(config_home) / "OrcaSlicer" if config_home else Path.home() / ".config/OrcaSlicer"


def _embedded_config_path(model_path: Path) -> str | None:
    try:
        with ZipFile(model_path) as archive:
            settings = json.loads(archive.read("Metadata/project_settings.config"))
    except KeyError:
        return None
    except (BadZipFile, json.JSONDecodeError) as error:
        raise ValueError(f"Cannot read embedded 3MF project settings: {error}") from error

    value = settings.get("dynapin_config_path") if isinstance(settings, dict) else None
    return value.strip() if isinstance(value, str) and value.strip() else None


def resolve_pull_config(model_path: Path, explicit_path: Path | None) -> Path:
    if explicit_path is not None:
        config_path = explicit_path.expanduser().resolve()
        if not config_path.is_file():
            raise FileNotFoundError(f"DynaPin config file not found: {config_path}")
        return config_path

    embedded_path = _embedded_config_path(model_path)
    if embedded_path is None:
        if DEFAULT_CONFIG.is_file():
            return DEFAULT_CONFIG
        raise FileNotFoundError(f"No dynapin_config_path in the 3MF and default config is missing: {DEFAULT_CONFIG}")

    requested_path = Path(embedded_path).expanduser()
    if requested_path.is_absolute():
        candidates = [requested_path]
    else:
        data_dir = _orca_data_dir()
        candidates = [
            data_dir / "user" / requested_path,
            data_dir / "system" / requested_path,
            REPOSITORY_ROOT / "resources/profiles" / requested_path,
            REPOSITORY_ROOT / "resources" / requested_path,
            Path.cwd() / requested_path,
        ]

    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()

    searched = "\n".join(f"  {candidate}" for candidate in candidates)
    raise FileNotFoundError(
        f"The 3MF references DynaPin config '{embedded_path}', but it was not found. "
        f"Pass --config with the matching JSON file. Searched:\n{searched}"
    )


def extract_pins(gcode: bytes) -> list[tuple[int, int]]:
    pins = sorted({(int(match.group(1)), int(match.group(2))) for match in PULL_BEGIN_RE.finditer(gcode)})
    if not pins:
        raise ValueError("No DynaPin pull pins found in the sliced G-code.")
    return pins


def write_bytes_atomically(destination: Path, content: bytes) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{destination.name}.",
            suffix=".tmp",
            dir=destination.parent,
            delete=False,
        ) as temporary_file:
            temporary_path = Path(temporary_file.name)
            temporary_file.write(content)
            temporary_file.flush()
            os.fsync(temporary_file.fileno())
        os.replace(temporary_path, destination)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def generate_pull_gcode(
    pins: Sequence[tuple[int, int]],
    base_file: Path,
    config_file: Path,
    destination: Path,
    pull_type: str | None,
    current: bool,
) -> None:
    pin_argument = "; ".join(f"{row},{col}" for row, col in pins)
    destination.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix=".dynapin-pull-", dir=destination.parent) as temporary_dir:
        temporary_output = Path(temporary_dir) / destination.name
        command = [
            sys.executable,
            str(PULL_GENERATOR),
            "--pins",
            pin_argument,
            "--base-file",
            str(base_file),
            "--config",
            str(config_file),
            "--output",
            str(temporary_output),
        ]
        if current:
            command.append("--current")
        elif pull_type:
            command.extend(("--type", pull_type))

        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if completed.stdout:
            print(completed.stdout, end="" if completed.stdout.endswith("\n") else "\n")
        if completed.returncode != 0:
            raise RuntimeError(f"Pull G-code generator exited with status {completed.returncode}.")
        if not temporary_output.is_file():
            raise RuntimeError("Pull G-code generator did not create its output file.")
        os.replace(temporary_output, destination)


def parse_args(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Slice one 3MF and write the full G-code, a minimal DynaPin G-code, "
            "and regenerated pin-pull G-code beside the 3MF."
        ),
        epilog=(
            "Example: python3 scripts/prepare_dynapin_3mf.py models/Arch_Bridge/PLA.3mf"
        ),
    )
    parser.add_argument("model", type=Path, help="single-plate 3MF input")
    parser.add_argument("--slicer", help="path or command name of the OrcaSlicer executable")
    parser.add_argument(
        "--base-file",
        type=Path,
        default=DEFAULT_BASE_FILE,
        help=f"base G-code copied before min/pull output (default: {DEFAULT_BASE_FILE})",
    )
    parser.add_argument(
        "--config",
        type=Path,
        help="DynaPin JSON for pull regeneration (default: config path embedded in the 3MF)",
    )
    pull_modes = parser.add_mutually_exclusive_group()
    pull_modes.add_argument("--type", choices=("normal", "alternating", "reverted"), help="override pull_gcode.type")
    pull_modes.add_argument("--current", action="store_true", help="use the generator's legacy comparison motion")
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    args = parse_args(arguments)
    model_path = args.model.expanduser().resolve()
    if not model_path.is_file():
        print(f"error: 3MF file does not exist: {model_path}", file=sys.stderr)
        return 2
    if model_path.suffix.casefold() != ".3mf":
        print(f"error: expected a .3mf file: {model_path}", file=sys.stderr)
        return 2

    base_file = args.base_file.expanduser().resolve()
    if not base_file.is_file():
        print(f"error: base G-code does not exist: {base_file}", file=sys.stderr)
        return 2

    sliced_gcode = model_path.with_suffix(".gcode")
    min_gcode = model_path.with_name(f"min_{model_path.stem}.gcode")
    pull_gcode = model_path.with_name(f"py_pull_{model_path.stem}.gcode")

    try:
        slicer = resolve_slicer(args.slicer, REPOSITORY_ROOT)
        config_file = resolve_pull_config(model_path, args.config)
    except (FileNotFoundError, OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(f"Using OrcaSlicer: {slicer}")
    print(f"Slicing: {model_path}")
    slice_result = slice_one(slicer, model_path, sliced_gcode)
    if not slice_result.success:
        print(f"error: slicing failed: {slice_result.message}", file=sys.stderr)
        return 1
    if slice_result.message:
        print(slice_result.message)

    try:
        source_content = sliced_gcode.read_bytes()
        base_content = base_file.read_bytes()
        min_content = extract_min_payload(base_content, source_content)
        if min_content is None:
            raise ValueError("No DynaPin pull markers found in the sliced G-code.")
        pins = extract_pins(source_content)
        write_bytes_atomically(min_gcode, min_content)
        generate_pull_gcode(
            pins,
            base_file,
            config_file,
            pull_gcode,
            args.type,
            args.current,
        )
    except (ExtractionError, FileNotFoundError, OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print(f"created: {sliced_gcode}")
    print(f"created: {min_gcode}")
    print(f"created: {pull_gcode}")
    print(f"created: {sliced_gcode}.slice_statistics.json")
    print(f"pins: {', '.join(f'{row},{col}' for row, col in pins)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
