# Batch-slice model 3MF files by filament Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Python standard-library command that slices each root-level `models/*.3mf` with the four Bambu filament presets and writes one G-code file per material under `models/outputs/<model>/`.

**Architecture:** Keep the batch orchestration in `scripts/slice_models_by_filament.py`. Separate model discovery, profile discovery/matching, slicer resolution, command construction, and atomic publication into small functions so they can be tested without a real OrcaSlicer process. Use the 3MF as the source of every setting except the filament profile supplied through `--load-filaments`.

**Tech Stack:** Python 3 standard library (`argparse`, `json`, `pathlib`, `subprocess`, `tempfile`, `shutil`, `unittest`); OrcaSlicer CLI.

---

## File map

- Create: `scripts/slice_models_by_filament.py` — cross-platform CLI, profile/executable discovery, slicing loop, error summary, and output publication.
- Create: `tests/scripts/test_slice_models_by_filament.py` — deterministic unit tests with temporary directories and mocked subprocess execution.
- Modify: `docs/superpowers/specs/2026-08-28-slice-models-by-filament-design.md` — record the corrected Bambu preset names and suffix matching rule.

### Task 1: Add tests for model and profile discovery

**Files:**

- Create: `tests/scripts/test_slice_models_by_filament.py`
- Create: `scripts/slice_models_by_filament.py`

- [ ] **Step 1: Add the test module loader and fixture helpers**

Use `importlib.util.spec_from_file_location` so the test can import a standalone script without requiring `scripts` to be a Python package. Add fixture JSON files through a helper that writes valid filament records.

```python
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


SCRIPT_PATH = Path(__file__).resolve().parents[2] / "scripts" / "slice_models_by_filament.py"
spec = importlib.util.spec_from_file_location("slice_models_by_filament", SCRIPT_PATH)
batch = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(batch)


class BatchSliceTests(unittest.TestCase):
    def write_profile(self, root, filename, name):
        path = root / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"type": "filament", "name": name}), encoding="utf-8")
        return path
```

- [ ] **Step 2: Write failing tests for root-only model discovery and Bambu profile matching**

```python
    def test_discover_models_ignores_child_directories_and_non_3mf_files(self):
        with tempfile.TemporaryDirectory() as temp:
            models = Path(temp)
            (models / "top.3mf").touch()
            (models / "README.txt").touch()
            (models / "nested").mkdir()
            (models / "nested" / "ignored.3mf").touch()

            self.assertEqual([models / "top.3mf"], batch.discover_models(models))

    def test_resolve_profiles_prefers_system_over_base_suffix(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            system = self.write_profile(root, "Bambu PLA Basic @System.json", "Bambu PLA Basic @System")
            self.write_profile(root, "Bambu PLA Basic @base.json", "Bambu PLA Basic @base")
            self.write_profile(root, "Bambu ABS @System.json", "Bambu ABS @System")
            self.write_profile(root, "Bambu PETG Basic @System.json", "Bambu PETG Basic @System")
            self.write_profile(root, "Bambu TPU-AMS @System.json", "Bambu TPU-AMS @System")

            profiles = batch.discover_profiles([root])
            resolved, errors = batch.resolve_filament_profiles(profiles, batch.FILAMENT_NAMES)

            self.assertEqual(system, resolved["Bambu PLA Basic"])
            self.assertFalse(errors)

    def test_resolve_profiles_reports_missing_and_ambiguous_names(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.write_profile(root, "one.json", "Bambu ABS")
            self.write_profile(root, "two.json", "Bambu ABS")

            profiles = batch.discover_profiles([root])
            resolved, errors = batch.resolve_filament_profiles(profiles, ["Bambu ABS", "Bambu TPU-AMS"])

            self.assertNotIn("Bambu ABS", resolved)
            self.assertIn("Bambu ABS", errors)
            self.assertIn("Bambu TPU-AMS", errors)
```

- [ ] **Step 3: Run the focused tests and verify they fail because the script API is absent**

Run: `uv run python -m unittest tests/scripts/test_slice_models_by_filament.py -v`

Expected: FAIL with import or missing-function errors for `discover_models`, `discover_profiles`, and `resolve_filament_profiles`.

### Task 2: Implement discovery and executable/command resolution

**Files:**

- Modify: `scripts/slice_models_by_filament.py`
- Test: `tests/scripts/test_slice_models_by_filament.py`

- [ ] **Step 1: Implement constants, profile records, and root-only model discovery**

Add the fixed material names and a `FilamentProfile` dataclass. `discover_models` must use `Path.iterdir()` rather than `rglob()` so `models/outputs` and other child directories cannot become inputs.

```python
FILAMENT_NAMES = (
    "Bambu PLA Basic",
    "Bambu ABS",
    "Bambu PETG Basic",
    "Bambu TPU-AMS",
)


@dataclass(frozen=True)
class FilamentProfile:
    name: str
    path: Path


def discover_models(models_dir: Path) -> list[Path]:
    return sorted(
        path for path in models_dir.iterdir()
        if path.is_file() and path.suffix.casefold() == ".3mf"
    )
```

- [ ] **Step 2: Implement JSON profile discovery and deterministic name matching**

Parse only JSON objects whose top-level `type` is `filament` and whose `name` is a string. Ignore malformed/non-profile JSON files. Match exact names first, then case-insensitive names, then names with a trailing `@System` or `@base` suffix removed. Prefer `@System` over no suffix and `@base`; return an error for equally ranked duplicates.

```python
def discover_profiles(roots: Iterable[Path]) -> list[FilamentProfile]:
    profiles = []
    seen = set()
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*.json")):
            if path in seen:
                continue
            seen.add(path)
            try:
                data = json.loads(path.read_text(encoding="utf-8-sig"))
            except (OSError, json.JSONDecodeError):
                continue
            if data.get("type") == "filament" and isinstance(data.get("name"), str):
                profiles.append(FilamentProfile(data["name"], path))
    return profiles


def resolve_filament_profiles(profiles, requested_names):
    resolved, errors = {}, {}
    for requested in requested_names:
        exact = [p for p in profiles if p.name.strip() == requested]
        folded = exact or [p for p in profiles if p.name.strip().casefold() == requested.casefold()]
        candidates = folded or [
            p for p in profiles
            if strip_profile_suffix(p.name).casefold() == requested.casefold()
        ]
        if not candidates:
            errors[requested] = "profile not found"
            continue
        best_rank = min(profile_match_rank(p.name, requested) for p in candidates)
        best = [p for p in candidates if profile_match_rank(p.name, requested) == best_rank]
        if len(best) != 1:
            errors[requested] = "ambiguous profiles: " + ", ".join(str(p.path) for p in best)
        else:
            resolved[requested] = best[0].path
    return resolved, errors
```

- [ ] **Step 3: Add tests for platform roots, executable lookup, and CLI arguments**

```python
    def test_default_profile_roots_use_windows_appdata(self):
        roots = batch.default_profile_roots(
            platform_name="win32",
            environ={"APPDATA": r"C:\\Users\\tester\\AppData\\Roaming"},
            home=Path(r"C:\\Users\\tester"),
        )
        self.assertEqual(Path(r"C:\\Users\\tester\\AppData\\Roaming") / "OrcaSlicer", roots[0])

    def test_build_slice_command_preserves_paths_as_arguments(self):
        command = batch.build_slice_command(
            Path("orca-slicer"),
            Path("models/part one.3mf"),
            Path("profiles/Bambu PLA Basic @System.json"),
            Path("temp/output"),
        )
        self.assertEqual(
            [
                "orca-slicer", "--slice", "0", "--load-filaments",
                "profiles/Bambu PLA Basic @System.json", "--outputdir",
                "temp/output", "models/part one.3mf",
            ],
            [str(value) for value in command],
        )
```

- [ ] **Step 4: Implement platform-aware profile roots and slicer lookup**

Use `ORCASLICER_DATA_DIR` when set, otherwise macOS `~/Library/Application Support/OrcaSlicer` or Windows `%APPDATA%/OrcaSlicer`. Resolve an explicit `--slicer`, then `orca-slicer`/`OrcaSlicer` on `PATH`, then conventional application paths. Keep all command arguments as a list so spaces in filenames work on both platforms.

- [ ] **Step 5: Run the focused tests and verify they pass**

Run: `uv run python -m unittest tests/scripts/test_slice_models_by_filament.py -v`

Expected: discovery, matching, root selection, and command construction tests PASS.

### Task 3: Add safe G-code publication and mocked slice execution

**Files:**

- Modify: `scripts/slice_models_by_filament.py`
- Modify: `tests/scripts/test_slice_models_by_filament.py`

- [ ] **Step 1: Add tests for successful publication, extra plates, and failed slices**

```python
    def test_publish_gcode_replaces_final_only_after_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "plate_1.gcode"
            destination = root / "outputs" / "part" / "Bambu ABS.gcode"
            source.write_text("new", encoding="utf-8")

            batch.publish_gcode(source, destination)

            self.assertEqual("new", destination.read_text(encoding="utf-8"))

    def test_slice_one_rejects_multiple_plate_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            model = root / "part.3mf"
            model.touch()
            profile = root / "Bambu ABS.json"
            profile.write_text("{}", encoding="utf-8")
            final = root / "outputs" / "part" / "Bambu ABS.gcode"
            final.parent.mkdir(parents=True)
            final.write_text("old", encoding="utf-8")

            def fake_run(command, **kwargs):
                output_dir = Path(command[command.index("--outputdir") + 1])
                (output_dir / "plate_1.gcode").write_text("one", encoding="utf-8")
                (output_dir / "plate_2.gcode").write_text("two", encoding="utf-8")
                return SimpleNamespace(returncode=0, stdout="")

            result = batch.slice_one(model, "Bambu ABS", profile, final, fake_run)

            self.assertFalse(result.success)
            self.assertEqual("old", final.read_text(encoding="utf-8"))
```

- [ ] **Step 2: Implement `build_slice_command`, `publish_gcode`, and `slice_one`**

`slice_one` should create a `TemporaryDirectory`, run `subprocess.run(command, stdout=PIPE, stderr=STDOUT, text=True, encoding="utf-8", errors="replace")`, reject nonzero return codes, require exactly one `plate_1.gcode`, and copy it into a temporary file in the final directory before `os.replace`. Return a small result record containing `success`, model, material, and diagnostic text so the outer loop can continue. The command must use OrcaSlicer's public `--load-filaments` spelling.

- [ ] **Step 3: Run the mocked execution tests**

Run: `uv run python -m unittest tests/scripts/test_slice_models_by_filament.py -v`

Expected: publication and failure-isolation tests PASS, including preservation of an older final G-code when the new slice is invalid.

### Task 4: Implement the command-line entry point and batch summary

**Files:**

- Modify: `scripts/slice_models_by_filament.py`
- Modify: `tests/scripts/test_slice_models_by_filament.py`

- [ ] **Step 1: Implement argument parsing and repository resolution**

Support `--repo-root PATH` and `--slicer PATH`; default the repository root to `Path(__file__).resolve().parents[1]`. Require `models/` to exist and report a nonzero status when it contains no root-level 3MF files.

- [ ] **Step 2: Implement `main` with preflight validation and continue-on-error behavior**

Preflight all four Bambu profile names and the slicer before starting. For every model/material pair, call `slice_one`, print the selected profile and result, retain diagnostics for failures, and continue to the next pair. Return `0` only when all `len(models) * 4` jobs succeed; return `2` for preflight/input errors and `1` for slice failures.

- [ ] **Step 3: Add a CLI integration test with a fake runner**

Patch the module's runner in the test to create `plate_1.gcode` and return success for all four material commands. Assert that the four files are created under `models/outputs/part/` with the Bambu material names, and assert that a nonzero fake runner produces a nonzero batch result without deleting successful outputs.

- [ ] **Step 4: Run syntax and all focused tests**

Run: `uv run python -m py_compile scripts/slice_models_by_filament.py`

Expected: no output and exit status `0`.

Run: `uv run python -m unittest tests/scripts/test_slice_models_by_filament.py -v`

Expected: all tests PASS.

### Task 5: Final verification and handoff

**Files:**

- Verify: `scripts/slice_models_by_filament.py`
- Verify: `tests/scripts/test_slice_models_by_filament.py`
- Verify: `docs/superpowers/specs/2026-08-28-slice-models-by-filament-design.md`

- [ ] **Step 1: Review the final diff for scope and corrected names**

Run: `git diff -- scripts/slice_models_by_filament.py tests/scripts/test_slice_models_by_filament.py docs/superpowers/specs/2026-08-28-slice-models-by-filament-design.md`

Confirm that only root-level `models/*.3mf` files are selected, all four names are exactly `Bambu PLA Basic`, `Bambu ABS`, `Bambu PETG Basic`, and `Bambu TPU-AMS`, and no DynaPin or vendored files are changed.

- [ ] **Step 2: Run a real CLI dry-run when a compatible binary and all four profiles are available**

Run: `uv run python scripts/slice_models_by_filament.py --slicer /path/to/orca-slicer`

Expected: each `models/<name>.3mf` produces the four requested paths. If the local installation lacks one of the four named profiles, the script stops in preflight with a clear missing-profile message rather than using a different material.

- [ ] **Step 3: Report the implementation and environment limitation**

Mention the command, output layout, test results, and whether a real slice was run. Preserve unrelated pre-existing worktree changes.
