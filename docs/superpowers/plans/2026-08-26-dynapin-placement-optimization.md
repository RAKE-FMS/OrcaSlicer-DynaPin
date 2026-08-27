# DynaPin Placement Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automatically find and apply the Z rotation and Y translation of one selected DynaPin model instance that minimizes geometry-only Normal support volume without changing X.

**Architecture:** Keep placement search in `libslic3r/DynaPin.cpp` operating on isolated model/print snapshots. Split Normal support generation at the geometry stage so candidates are scored before toolpath generation, then use a GUI job to apply only the winning instance on the UI thread and restart slicing once.

**Tech Stack:** C++17, Eigen transforms, existing OrcaSlicer polygon/ExPolygon operations, Catch2 tests, GUI `Job`/`Plater` worker.

---

### Task 1: Add and validate pin-tip collision configuration

**Files:**
- Modify: `src/libslic3r/DynaPin.hpp`
- Modify: `src/libslic3r/DynaPin.cpp`
- Modify: `resources/profiles/Kingroon/dynapin/kp3s.json`
- Test: `tests/libslic3r/test_dynapin_preview.cpp`

- [x] Add a `TipCollisionConfig` with `x_min`, `x_max`, `width_y`, `thickness_z`, and `clearance`, and store it in `DynaPin::Config`.
- [x] Parse and validate `tip_collision`; reject non-finite or non-positive dimensions while preserving the existing config error format.
- [x] Add `tip_collision_box_for_pin()` that uses the physical support-pin Y center, `pin_z()` as the Z center, and returns a machine-coordinate box expanded by `clearance`.
- [x] Register KP3S values `0`, `20`, `12.4`, `5`, and `1` in the profile.
- [x] Add tests for the pin-tip box bounds and clearance.
- [x] Run the focused DynaPin tests after the test target exists.

### Task 2: Extract geometry-only Normal support scoring

**Files:**
- Modify: `src/libslic3r/Support/SupportMaterial.hpp`
- Modify: `src/libslic3r/Support/SupportMaterial.cpp`
- Modify: `src/libslic3r/Support/SupportCommon.cpp`
- Test: `tests/libslic3r/test_dynapin_preview.cpp`

- [x] Add a geometry-evaluation mode that runs DynaPin selection, blocker clipping, contact/interface/base/intermediate region generation, and pin-top propagation handling but skips `generate_support_toolpaths()`.
- [x] Calculate volume by sweeping all support-layer Z boundaries, unioning active polygons for each slab, and multiplying area by slab height.
- [x] Include top contacts, bottom contacts, intermediate, interface, and base-interface regions; exclude raft layers.
- [x] Keep full slicing behavior unchanged by using the new mode only for optimizer candidates.
- [x] Test overlapping polygons, different slab heights, and raft exclusion with deterministic polygons.

### Task 3: Implement placement candidate generation and constraints

**Files:**
- Modify: `src/libslic3r/DynaPin.hpp`
- Modify: `src/libslic3r/DynaPin.cpp`
- Test: `tests/libslic3r/test_dynapin_preview.cpp`

- [x] Define result/callback types for one object instance and isolated fixed-scene evaluation.
- [x] Generate relative rotations `0..355°` at 5° and refine the five lowest-volume basins at 1°.
- [x] Generate Δy at `pitch/16` in the coarse pass and refine locally around the best coarse candidates.
- [x] Always include `(rotation=0, delta_y=0)` when it is feasible and deduplicate evaluated poses.
- [x] Compute the feasible Δy interval per rotation and transfer clipped width to the opposite side where possible.
- [x] Reject build-volume violations, fixed-model collisions, and configured tip-collision boxes; do not search an independent Δx.
- [x] Compare by support volume, then `abs(delta_y)`, then smallest angular displacement, with the `max(1 mm³, 0.1%)` equality tolerance.
- [ ] Add isolated deterministic tests for interval transfer and optimizer tie-breaking.

### Task 4: Integrate isolated candidate evaluation with Print support generation

**Files:**
- Modify: `src/libslic3r/Print.hpp`
- Modify: `src/libslic3r/Print.cpp`
- Modify: `src/libslic3r/PrintObject.cpp`
- Modify: `src/libslic3r/Support/SupportMaterial.cpp`

- [x] Construct a temporary print state for each candidate and invoke the geometry-only support mode.
- [x] Recompute Auto DynaPin selection for every candidate and preserve existing selected-pin, blocker, and pin-top behavior.
- [x] Ensure candidate evaluation does not mutate the live UI model, global DynaPin selection, or cached full-slice result.
- [x] Propagate cancellation and invalid-config/no-feasible-pose statuses without leaving partial support state in the live print.
- [ ] Reuse rotated slices across Δy candidates; the current implementation favors a simpler isolated evaluation path.
- [ ] Add integration coverage showing that the selected instance changes while other instances remain unchanged.

### Task 5: Add automatic GUI job and safe application

**Files:**
- Create: `src/slic3r/GUI/Jobs/DynaPinPlacementJob.hpp`
- Create: `src/slic3r/GUI/Jobs/DynaPinPlacementJob.cpp`
- Modify: `src/slic3r/GUI/Plater.cpp`
- Modify: `src/slic3r/CMakeLists.txt`

- [x] Capture exactly one selected object/instance and a model/config snapshot before starting the worker.
- [x] Start the job only for DynaPin enabled, Auto pins, Normal support, and a single selected instance; otherwise continue normal slicing.
- [x] Report progress and allow cancellation through the existing `Job::Ctl` interface.
- [x] On success, take one Undo snapshot on the UI thread, apply Z rotation and Y translation, call `ensure_on_bed()`, and restart slicing.
- [x] Add a one-shot guard to prevent recursive re-entry during the optimizer-triggered reslice.
- [ ] Add a persistent input-signature cache for repeated unchanged updates.
- [x] Show a warning for initial pin-tip collision and leave the model unchanged for cancellation, invalid configuration, or no feasible pose.

### Task 6: Regression and fixture validation

**Files:**
- Modify: `tests/fff_print/test_print.cpp`
- Modify: `tests/libslic3r/test_dynapin_preview.cpp`

- [x] Preserve and run the completed DynaPin Z-rotation invalidation test.
- [ ] Add automatic-trigger, recursion-guard, selected-instance-only, and cancellation tests.
- [x] Preserve the existing regression coverage for blocker Z ranges and upper-support termination at pin tops.
- [x] Run the focused libslic3r/FFF tests and rebuild the GUI library.
- [ ] Slice `gymnast.3mf` manually and record before/after support geometry volume, Δy, rotation, selected pins, and elapsed time.

## Review follow-ups

The staged implementation review found the following items still open. The placement optimization work must not be considered fully validated until these are addressed:

- [x] Prove that splitting projections per contact cannot reintroduce an upper overhang below a selected pin's `blocker.z_min`, while preserving genuinely independent lower overhangs. Safe pin landings now terminate their projection permanently; only later independent contacts create new states.
- [ ] Replace the current DynaPin support fixtures with fixtures whose support geometry actually overlaps the selected pin landing/blocker regions; assert both blocked and intentionally preserved support.
- [ ] Verify that the per-contact grid projection does not change standard-support geometry or cause an unacceptable runtime increase; retain the aggregate fast path when DynaPin is disabled if needed.
- [ ] Make automatic pin detection side-effect-free with respect to `Layer::sharp_tails`, or deduplicate repeated registrations.
- [ ] Apply `dynapin_debug_stage` consistently to selection, support generation, and pull G-code emission, including environment-variable overrides.
- [ ] Correct the diagnostic per-layer BoundingBox aggregation.
