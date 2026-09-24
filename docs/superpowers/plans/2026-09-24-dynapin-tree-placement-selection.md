# Tree DynaPin placement selection implementation plan

**Goal:** After position and rotation optimization, slice non-Organic Tree support with automatically selected pins while preserving Normal support and DynaPin-disabled behavior.

**Architecture:** Trace the candidate evaluation and final reslice through `DynaPinPlacement.cpp`, `Print.cpp`, and `PrintObject.cpp`. Correct the first boundary where Tree pin selection or its landing plan diverges from the final pose. Keep Normal and disabled paths unchanged.

**Tech Stack:** C++17, Catch2, OrcaSlicer FFF print tests.

---

### Task 1: Reproduce and isolate

- [x] Compare Tree candidate evaluation (`evaluate_scene_candidate`, `evaluate_scene_angle_group`) with the final `Print::process()` path.
- [x] Add a deterministic failing regression in `tests/fff_print/test_print.cpp` or `tests/libslic3r/test_dynapin_placement.cpp` that applies an optimized transform, slices, and checks automatic pins and Tree landing use.
- [x] Run the focused test to confirm the failure and record the differing state at selection, Tree generation, and finalization.

### Task 2: Fix the cause

- [x] Make the smallest production change at the proven boundary. Do not alter Normal selection or support generation when DynaPin is disabled.
- [x] Run the new test until it passes and inspect the complete diff for unrelated changes.

### Task 3: Verify behavior

- [x] Run focused Tree placement and Tree selection tests.
- [x] Run existing Normal placement and support tests, plus DynaPin-disabled support tests.
- [x] Confirm the optimized Tree pose produces selected pins, matching Tree landings, and support extrusion through `Print::process()`.

No commits are permitted.

## Follow-up verification

An explicit long-running regression in `tests/fff_print/test_print.cpp` constrains the search to the fixture's useful Y interval, runs `run_placement_task()`, applies its improved pose, and calls `Print::process()`. It passed with automatic pins, matching Tree landing pins, and nonempty support extrusion paths. A separate G-code export attempt with this synthetic `DynamicPrintConfig::full_print_config()` fixture crashed while serializing the `extruder_type` enum in `GCode::append_full_config()`. Actual G-code output was verified later through CLI slicing with the user's complete project configuration.

## Real workflow regression (follow-up)

The user supplied `models/Simple_Bridge/PLA.3mf`. With its printer settings and Tree support, a CLI slice with placement disabled selected three pins and used 1.381932582 g of support filament. The same pose with DynaPin disabled selected no pins and used 2.202987959 g. These numbers establish a real material reduction that the optimized result must preserve.

The missing case was an automatic Tree candidate with zero projected pins. Before the fix, a no-overhang Tree fixture received a valid zero-volume placement score; its focused regression failed. The candidate scorer now rejects zero projected pins or zero usable Tree landings. The regression passes, alongside Normal evaluator parity, DynaPin-disabled Tree support, and Tree landing checks. The Release app built successfully. The constrained optimization plus reslice regression passed (7 assertions).

The unrestricted CLI optimizer selected relative rotation 355 degrees and Y shift 0 on the user project. The final optimized G-code emitted three DynaPin pull moves and used 1.3576663875 g support filament. The CLI's `--export-3mf` retained the original model transform, so the same-pose comparison used an original 3MF copy with its build item rotated from absolute 90 to 85 degrees around the same center, with placement search disabled. That pose reproduced the optimized 1.3576663875 g and three pull moves. Disabling DynaPin at the identical pose yielded 2.1816619321 g and zero pull moves: 0.8239955446 g (37.8%) more support filament. The exported 3MF behavior is outside this fix but matters when reproducing the comparison.

A bounded regression imports a copy of the supplied 3MF, evaluates the original and observed winning rotation, applies the optimized pose, and reslices. It asserts exact pins `{1,5}`, `{1,6}`, `{1,7}`, matching Tree landings and support paths, plus a support volume at least 10% below DynaPin-disabled at the same pose. It passed with 15 assertions.

- [x] Reproduce the reported case with unrestricted placement optimization through the CLI entry point, recording the selected pose, final selected pin count from G-code, and support material usage.
- [x] Compare the same pose and settings with DynaPin enabled and disabled; verify a meaningful support material difference when pins are used.
- [x] Identify the zero-pin Tree candidate scoring gap and fix it while preserving Normal and disabled behavior.
- [x] Keep a regression that runs optimization, applies its chosen pose, slices, and checks final pins, support geometry, and material comparison. Verify G-code output through the CLI with the complete project configuration.
- [x] Rebuild, run focused Tree/Normal/disabled regressions, inspect the diff, and leave all changes uncommitted.
