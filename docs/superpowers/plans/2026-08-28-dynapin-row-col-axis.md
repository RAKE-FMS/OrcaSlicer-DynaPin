# DynaPin row/column axis correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct DynaPin address semantics so `row` indexes vertical Z levels and `col` indexes horizontal Y positions throughout the slicer.

**Architecture:** Keep `DynaPin::Pin{row, col}` and preview comment syntax unchanged, but make every coordinate consumer use `row_z` and `col_y`. Rename pitch fields and JSON keys so the schema documents the corrected semantics; update the bundled KP3S dimensions and all focused fixtures together.

**Tech Stack:** C++17, Catch2, nlohmann/json, OrcaSlicer support-material geometry, Markdown/JSON printer profile data.

---

### Task 1: Correct the DynaPin coordinate model and loader

**Files:**
- Modify: `src/libslic3r/DynaPin.hpp`
- Modify: `src/libslic3r/DynaPin.cpp`

- [ ] Rename `Config::row_pitch_y` to `row_pitch_z` and `Config::col_pitch_z` to `col_pitch_y`.
- [ ] Change `pin_y()` to `support_origin_y + col * col_pitch_y`.
- [ ] Change `blocker_z_range()` to use `support_origin_z + row * row_pitch_z`.
- [ ] Change pull helpers to use `col` for Y and `row` for Z.
- [ ] Keep candidate enumeration and pin parsing in `(row, col)` order, but make their count ranges represent Z rows and Y columns.
- [ ] Read `pitch.row_z` and `pitch.col_y`; do not accept `row_y` or `col_z` aliases.
- [ ] Update validation and diagnostic labels so pitch output remains understandable.

Expected core formulas:

```cpp
double pin_y(const Config& config, const Pin& pin)
{ return config.support_origin_y + double(pin.col) * config.col_pitch_y; }

const double z_max = config.support_origin_z + double(pin.row) * config.row_pitch_z;

static double pull_y(const Config& config, const Pin& pin)
{ return config.pull_origin_y + double(pin.col) * config.col_pitch_y + config.pull_gcode.y_offset; }

static double pull_z(const Config& config, const Pin& pin)
{ return config.pull_origin_z + double(pin.row) * config.row_pitch_z; }
```

### Task 2: Update the bundled KP3S profile and DynaPin documentation

**Files:**
- Modify: `resources/profiles/Kingroon/dynapin/kp3s.json`
- Modify: `resources/profiles/Kingroon/dynapin/README.md`
- Modify: `docs/dynapin-support-optimization-plan.ja.md`
- Modify: `docs/dynapin-support-optimization-plan.en.md`
- Modify: `docs/dynapin-prepare-selection-plan.ja.md`
- Modify: `docs/superpowers/specs/2026-08-25-dynapin-origin-naming-design.md`
- Modify: `docs/superpowers/plans/2026-08-25-dynapin-origin-naming.md`
- Modify: `docs/superpowers/specs/2026-08-27-dynapin-support-z-naming-design.md`
- Modify: `docs/superpowers/plans/2026-08-27-dynapin-support-z-naming.md`

- [ ] Set KP3S to `row_count=14`, `col_count=10`, `row_z=7.4`, and `col_y=12.4`, preserving the current physical Y/Z spacing and local uncommitted profile edits unrelated to this change.
- [ ] Rewrite diagrams, formulas, tables, and examples to state `row → Z` and `col → Y`.
- [ ] Replace old pitch field names in implementation plans/specs so future work does not restore the old mapping.

### Task 3: Update focused unit and support tests

**Files:**
- Modify: `tests/libslic3r/test_dynapin_preview.cpp`
- Modify: `tests/fff_print/test_print.cpp`
- Modify: `tests/fff_print/test_support_material.cpp`

- [ ] Rename test fixture fields to `row_pitch_z` and `col_pitch_y`.
- [ ] Add/adjust assertions proving changing row changes Z only and changing col changes Y only.
- [ ] Change grid endpoint expectations to `(13, 9)` for the 14-by-10 KP3S grid.
- [ ] Convert selected pin fixtures whose intent is a particular Z level or Y position to the corrected `(row, col)` address.
- [ ] Recalculate support blocker and virtual-surface Z assertions from the corrected row index while preserving their intended physical geometry.
- [ ] Keep preview parsing tests in `(row, col)` order; only coordinate calculations change.

### Task 4: Sweep and remove stale mapping references

**Files:**
- Modify any remaining DynaPin files found by `rg` in `src/`, `tests/`, `resources/profiles/`, and `docs/`.

- [ ] Run `rg -n -S 'row_pitch_y|col_pitch_z|row_y|col_z'` excluding unrelated third-party/UI table code.
- [ ] Ensure no DynaPin source, test, profile, or active design document still describes row as Y or col as Z.
- [ ] Verify G-code comments remain `ROW=<row> COL=<col>` and are not renamed.

### Task 5: Verify the correction

**Files:**
- No additional files.

- [ ] Run the focused DynaPin preview test target.
- [ ] Run the focused FFF print/support-material test targets or their available CTest filters.
- [ ] Build the affected targets with the existing `build/arm64` configuration.
- [ ] Review `git diff --check` and inspect the final diff to confirm unrelated dirty files remain untouched.
