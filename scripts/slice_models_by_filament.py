#!/usr/bin/env python3
"""Slice each 3MF below models using its embedded settings."""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence


PLATE_GCODE_RE = re.compile(r"plate_(\d+)\.gcode$", re.IGNORECASE)


@dataclass(frozen=True)
class SliceResult:
    success: bool
    model: Path
    message: str = ""


def discover_models(models_dir: Path) -> list[Path]:
    """Return all 3MF files below *models_dir* in stable order."""

    return sorted(
        path
        for path in models_dir.rglob("*")
        if path.is_file() and path.suffix.casefold() == ".3mf"
    )


def _existing_file(path: Path) -> Path | None:
    return path if path.is_file() else None


def _release_build_candidates(repo_root: Path, platform_name: str) -> list[Path]:
    """Return repository Release binaries, newest first.

    The install prefix (for example ``build/arm64/OrcaSlicer`` on macOS) is
    intentionally not searched here because it can contain a stale copy of a
    previously installed application and its bundled resources.
    """

    if platform_name == "darwin":
        build_patterns = (
            "build/**/src/Release/*.app/Contents/MacOS/orca-slicer",
            "build/**/src/Release/*.app/Contents/MacOS/OrcaSlicer",
            "build/**/Release/*.app/Contents/MacOS/orca-slicer",
            "build/**/Release/*.app/Contents/MacOS/OrcaSlicer",
        )
    elif platform_name.startswith("win"):
        build_patterns = (
            "build/**/src/Release/orca-slicer.exe",
            "build/**/src/Release/OrcaSlicer.exe",
            "build/**/Release/orca-slicer.exe",
            "build/**/Release/OrcaSlicer.exe",
        )
    else:
        build_patterns = (
            "build/**/src/Release/orca-slicer",
            "build/**/src/Release/OrcaSlicer",
            "build/**/Release/orca-slicer",
            "build/**/Release/OrcaSlicer",
        )

    candidates: list[Path] = []
    seen: set[str] = set()
    for pattern in build_patterns:
        for candidate in repo_root.glob(pattern):
            if not candidate.is_file():
                continue
            normalized = os.path.normcase(os.path.abspath(os.fspath(candidate)))
            if normalized in seen:
                continue
            seen.add(normalized)
            candidates.append(candidate)

    def sort_key(candidate: Path) -> tuple[int, str]:
        try:
            modified_ns = candidate.stat().st_mtime_ns
        except OSError:
            modified_ns = -1
        return modified_ns, os.fspath(candidate)

    return sorted(candidates, key=sort_key, reverse=True)


def resolve_slicer(
    explicit: str | os.PathLike[str] | None = None,
    repo_root: Path | None = None,
    platform_name: str | None = None,
    environ: Mapping[str, str] | None = None,
) -> Path:
    """Find an OrcaSlicer CLI executable."""

    platform_name = platform_name or sys.platform
    if environ is None:
        environ = os.environ

    if explicit:
        explicit_text = os.fspath(explicit)
        path_from_path = shutil.which(explicit_text)
        if path_from_path:
            return Path(path_from_path)
        explicit_path = Path(explicit_text).expanduser()
        if explicit_path.is_file():
            return explicit_path
        raise FileNotFoundError(f"OrcaSlicer executable not found: {explicit_text}")

    executable_names = (
        ("orca-slicer", "OrcaSlicer", "orca-slicer.exe", "OrcaSlicer.exe")
        if platform_name.startswith("win")
        else ("orca-slicer", "OrcaSlicer")
    )

    if repo_root:
        release_candidates = _release_build_candidates(repo_root, platform_name)
        if release_candidates:
            return release_candidates[0]

    for executable_name in executable_names:
        path_from_path = shutil.which(executable_name)
        if path_from_path:
            return Path(path_from_path)

    candidates: list[Path] = []

    if platform_name == "darwin":
        for applications_dir in (Path("/Applications"), home_applications_dir()):
            candidates.extend(
                applications_dir / "OrcaSlicer.app" / "Contents" / "MacOS" / name
                for name in ("orca-slicer", "OrcaSlicer")
            )
    elif platform_name.startswith("win"):
        install_roots = [
            environ.get("LOCALAPPDATA"),
            environ.get("PROGRAMFILES"),
            environ.get("PROGRAMFILES(X86)"),
        ]
        for install_root in install_roots:
            if install_root:
                candidates.extend(
                    Path(install_root) / "OrcaSlicer" / name
                    for name in ("orca-slicer.exe", "OrcaSlicer.exe")
                )

    for candidate in candidates:
        existing = _existing_file(candidate)
        if existing:
            return existing

    searched = ", ".join(executable_names)
    raise FileNotFoundError(f"Could not find OrcaSlicer ({searched}). Use --slicer PATH.")


def home_applications_dir() -> Path:
    return Path.home() / "Applications"


def build_slice_command(
    slicer: Path,
    model: Path,
    temporary_output: Path,
) -> list[str]:
    return [
        os.fspath(slicer),
        "--slice",
        "0",
        "--outputdir",
        os.fspath(temporary_output),
        os.fspath(model),
    ]


def publish_gcode(source: Path, destination: Path) -> None:
    """Atomically replace destination after copying source beside it."""

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_destination: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb",
            prefix=f".{destination.stem}.",
            suffix=".tmp",
            dir=destination.parent,
            delete=False,
        ) as temporary_file:
            temporary_destination = Path(temporary_file.name)
            with source.open("rb") as source_file:
                shutil.copyfileobj(source_file, temporary_file)
            temporary_file.flush()
            os.fsync(temporary_file.fileno())
        os.replace(temporary_destination, destination)
        temporary_destination = None
    finally:
        if temporary_destination is not None:
            try:
                temporary_destination.unlink()
            except FileNotFoundError:
                pass


def _diagnostics(completed_process: object) -> str:
    output = getattr(completed_process, "stdout", "") or ""
    return str(output).strip()


def slice_one(
    slicer: Path,
    model: Path,
    destination: Path,
    runner: Callable[..., object] = subprocess.run,
) -> SliceResult:
    """Slice one 3MF and publish its G-code only when it is valid."""

    try:
        with tempfile.TemporaryDirectory(prefix="orca-slicer-batch-") as temporary_dir:
            temporary_output = Path(temporary_dir)
            command = build_slice_command(slicer, model, temporary_output)
            completed_process = runner(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            diagnostics = _diagnostics(completed_process)
            return_code = int(getattr(completed_process, "returncode", 1))
            if return_code != 0:
                message = f"OrcaSlicer exited with status {return_code}."
                if diagnostics:
                    message += f"\n{diagnostics}"
                return SliceResult(False, model, message)

            plate_outputs = sorted(
                path
                for path in temporary_output.iterdir()
                if path.is_file() and PLATE_GCODE_RE.fullmatch(path.name)
            )
            expected_output = temporary_output / "plate_1.gcode"
            if len(plate_outputs) != 1 or plate_outputs[0] != expected_output:
                names = ", ".join(path.name for path in plate_outputs) or "none"
                return SliceResult(
                    False,
                    model,
                    "Expected exactly one single-plate output named plate_1.gcode; "
                    f"found: {names}",
                )

            publish_gcode(expected_output, destination)
            return SliceResult(True, model)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return SliceResult(False, model, str(error))


def run_batch(
    models_dir: Path,
    output_root: Path,
    slicer: Path,
    runner: Callable[..., object] = subprocess.run,
) -> list[SliceResult]:
    results: list[SliceResult] = []
    for model in discover_models(models_dir):
        destination = (output_root / model.relative_to(models_dir)).with_suffix(".gcode")
        print(f"[{model.relative_to(models_dir)}]")
        result = slice_one(slicer, model, destination, runner)
        if result.success:
            print(f"  OK: {destination}")
        else:
            print(f"  FAILED: {result.message}", file=sys.stderr)
        results.append(result)
    return results


def parse_args(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Slice every 3MF under models/ using its embedded settings."
    )
    parser.add_argument(
        "--repo-root",
        type=Path,
        help="repository root containing models/ (default: inferred from this script)",
    )
    parser.add_argument(
        "--slicer",
        help="path or command name of the OrcaSlicer executable",
    )
    return parser.parse_args(arguments)


def main(arguments: Sequence[str] | None = None) -> int:
    args = parse_args(arguments)
    repo_root = (args.repo_root or Path(__file__).resolve().parents[1]).expanduser().resolve()
    models_dir = repo_root / "models"
    output_root = repo_root / "outputs"

    if not models_dir.is_dir():
        print(f"Models directory not found: {models_dir}", file=sys.stderr)
        return 2

    models = discover_models(models_dir)
    if not models:
        print(f"No .3mf files found under {models_dir}", file=sys.stderr)
        return 2

    try:
        slicer = resolve_slicer(args.slicer, repo_root)
    except FileNotFoundError as error:
        print(str(error), file=sys.stderr)
        return 2

    print(f"Using OrcaSlicer: {slicer}")
    print(f"Found {len(models)} 3MF file(s) under {models_dir}")
    results = run_batch(models_dir, output_root, slicer)
    failures = [result for result in results if not result.success]
    print(f"Completed {len(results) - len(failures)}/{len(results)} slices.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
