# Batch-slice model 3MF files by filament

## Goal

Provide one Windows/macOS-compatible command file that batch-slices every `.3mf` file directly under `models/` four times, once for each filament preset name:

- `Bambu PLA Basic`
- `Bambu ABS`
- `Bambu PETG Basic`
- `Bambu TPU-AMS`

For each input `models/<model>.3mf`, write the successful result to:

```text
models/outputs/<model>/<filament>.gcode
```

Subdirectories under `models/` are intentionally not scanned.

## Chosen approach

Add `scripts/slice_models_by_filament.py`, implemented with Python's standard library only. Python provides the same file discovery, path handling, process execution, and JSON parsing behavior on Windows and macOS, without maintaining separate shell and batch implementations.

The script is run from any working directory and resolves the repository root from its own location. Its normal invocation is:

```text
uv run python scripts/slice_models_by_filament.py
```

The script also accepts an optional `--repo-root` path and an optional `--slicer` executable override so it remains usable with a relocated checkout or a nonstandard OrcaSlicer installation.

## Profile and executable discovery

The script searches the current user's OrcaSlicer data directory for JSON files and parses each file's top-level `name` property. The standard roots are:

- macOS: `~/Library/Application Support/OrcaSlicer`
- Windows: `%APPDATA%/OrcaSlicer`

For each requested filament, an exact profile-name match is preferred. The common `@System` and `@base` suffixes are ignored when matching, with the `@System` profile preferred over its corresponding `@base` profile. Matching is case-insensitive only when there is no exact match. If no profile matches, or more than one equally preferred profile matches, that material is reported as an error and is not sliced. The script does not silently choose an arbitrary vendor profile.

The slicer executable is resolved in this order:

1. `--slicer` when supplied;
2. `orca-slicer` / `OrcaSlicer` available on `PATH`;
3. conventional macOS application-bundle and Windows installation paths.

The script validates all required profiles and the executable before beginning work.

## Slicing and output flow

For every root-level `.3mf` file, and for every material in the fixed order above:

1. Create an isolated temporary output directory.
2. Invoke OrcaSlicer in CLI mode with:
   - `--slice 0` to slice all plates;
   - `--load-filaments <profile-json>` to override only the filament settings;
   - `--outputdir <temporary-directory>` to keep intermediate files out of `models/outputs`;
   - the 3MF path as input.
3. Require the expected single-plate result `plate_1.gcode`.
4. Move the completed G-code to `models/outputs/<model>/<material>.gcode` only after OrcaSlicer exits successfully.

The 3MF remains the source of printer, process, geometry, and other project settings. Output directories are created as needed. A successful rerun replaces the corresponding completed G-code atomically after the new slice finishes; a failed slice does not replace an older successful result. The public CLI option is `--load-filaments` (the underscore form is an internal configuration key and is not used by the command file).

The current CLI exports one G-code file per plate (`plate_1.gcode`, `plate_2.gcode`, ...). Since the requested output layout has one G-code file per 3MF/material pair, the script accepts only a single-plate 3MF. If additional plate G-codes are produced, it reports an actionable error and leaves the prior final output unchanged.

## Error handling and exit status

The script continues processing independent model/material pairs after a slicing failure, so one bad 3MF or incompatible material does not hide the remaining results. It prints each command's material and model context, captures OrcaSlicer's diagnostic output, and reports a summary of successes and failures at the end.

It exits with status `0` only when every discovered 3MF was successfully sliced for all four materials. It exits nonzero when there are no input 3MF files, a required profile or executable is missing, or any requested slice fails.

## Verification

Verification will cover:

- Python syntax compilation;
- unit-level checks for root-only `.3mf` discovery, profile-name matching, output-name derivation, and executable/profile validation using temporary fixtures;
- a CLI dry-run or mocked process test to confirm that each material receives the intended arguments and that failed slices do not replace final outputs;
- if a compatible local OrcaSlicer binary is available, a real one-model run against the repository's `models/` fixtures, checking the four output paths.
