# DynaPin Origin Naming and Zero-Based Grid Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename DynaPin coordinate origins to `pull_origin` and `support_origin`, remove configurable row/column origins, and make every grid start at `(0, 0)` without supporting the old JSON schema.

**Architecture:** Keep the existing DynaPin responsibilities and geometry unchanged. `pull_origin` remains the base for pull G-code, while `support_origin` remains the installed pin-array base used by support geometry and collision checks. Remove only the configurable origin indices and replace their arithmetic with zero-based row/column values.

**Tech Stack:** C++17, nlohmann::json, Catch2, CMake/CTest, Markdown and JSON printer profile resources.

---

### Task 1: Update unit tests to describe the new coordinate contract

**Files:**
- Modify: `tests/libslic3r/test_dynapin_preview.cpp:58-137`

- [x] **Step 1: Rename test fixture fields to the new C++ names**

Replace every direct assignment to `config.origin_y`, `config.origin_z`, `config.physical_origin_y`, `config.physical_origin_z`, `config.origin_row`, and `config.origin_col` with `pull_origin_y`, `pull_origin_z`, `support_origin_y`, and `support_origin_z` as applicable. Keep the existing expected G-code and physical coordinates unchanged because the configured numeric values and formulas remain equivalent.

- [x] **Step 2: Make the candidate-grid test assert the fixed zero-based contract**

Rename `DynaPin candidate grid starts at the configured origin` to `DynaPin candidate grid starts at zero`, remove the `origin_row` and `origin_col` setup, and assert that a `14 x 10` grid produces `{0, 0}` through `{13, 9}`. This ensures the test no longer implies that an origin index is configurable.

- [x] **Step 3: Update the sorting test to configure the support Z base**

Set `config.support_origin_z = 5.` in the physical-height sorting test. Keep the pin list and expected order aligned with the corrected mapping; the test should continue proving that sorting is based on `support_origin_z + row * row_pitch_z`.

- [x] **Step 4: Run the focused test target before implementation**

Run:

```bash
cmake --build build --target tests --parallel
ctest --test-dir build --output-on-failure -R 'DynaPin|dynapin'
```

Expected: compilation fails because the new `Config` field names do not exist yet. This confirms the test edits exercise the intended API rename.

### Task 2: Replace the DynaPin configuration model and coordinate calculations

**Files:**
- Modify: `src/libslic3r/DynaPin.hpp:1-90`
- Modify: `src/libslic3r/DynaPin.cpp:55-110`
- Modify: `src/libslic3r/DynaPin.cpp:258-334`

- [x] **Step 1: Replace the origin fields in `DynaPin::Config`**

Change the grid-related fields to:

```cpp
int    row_count = 0;
int    col_count = 0;
double pull_origin_y = 0.;
double pull_origin_z = 0.;
double support_origin_y = 0.;
double support_origin_z = 0.;
double row_pitch_z = 0.;
double col_pitch_y = 0.;
```

Remove `origin_row`, `origin_col`, `origin_y`, `origin_z`, and the optional physical-origin fields. Remove the now-unused `<optional>` include from the header if no other declaration uses it.

- [x] **Step 2: Implement zero-based coordinate helpers**

Use these formulas in `DynaPin.cpp`:

```cpp
double pin_y(const Config& config, const Pin& pin)
{ return config.support_origin_y + double(pin.col) * config.col_pitch_y; }

double pin_z(const Config& config, const Pin& pin)
{ return config.support_origin_z + double(pin.row) * config.row_pitch_z; }

static double pull_y(const Config& config, const Pin& pin)
{ return config.pull_origin_y + double(pin.col) * config.col_pitch_y + config.pull_gcode.y_offset; }

static double pull_z(const Config& config, const Pin& pin)
{ return config.pull_origin_z + double(pin.row) * config.row_pitch_z; }
```

Update `candidate_pins()` to loop with `row = 0; row < row_count` and `col = 0; col < col_count`. Do not modify `Pin::row` / `Pin::col` or selected-pin parsing.

- [x] **Step 3: Parse only `pull_origin` and `support_origin`**

In `load_config_for_print()`, read the two nested objects from `grid`:

```cpp
const nlohmann::json pull_origin = grid.contains("pull_origin") && grid["pull_origin"].is_object() ?
                                       grid["pull_origin"] : nlohmann::json::object();
config.pull_origin_y = number_or(pull_origin, "y", config.pull_origin_y);
config.pull_origin_z = number_or(pull_origin, "z", config.pull_origin_z);

const nlohmann::json support_origin = grid.contains("support_origin") && grid["support_origin"].is_object() ?
                                          grid["support_origin"] : nlohmann::json::object();
config.support_origin_y = number_or(support_origin, "y", config.support_origin_y);
config.support_origin_z = number_or(support_origin, "z", config.support_origin_z);
```

Remove all reads of `origin_row`, `origin_col`, `origin_y`, `origin_z`, `grid.origin`, and `grid.physical_origin`. Do not add fallback aliases.

- [x] **Step 4: Update configuration diagnostics**

Change the debug log labels to `pull_origin_mm=(...)` and `support_origin_mm=(...)`. Use `support_origin_y/z` for the exclusion center and Z range. Keep the existing validation and error behavior for dimensions and pull-front position unchanged.

- [x] **Step 5: Build the library/tests after the implementation change**

Run:

```bash
cmake --build build --target tests --parallel
```

Expected: the test target compiles successfully with the renamed `Config` fields and zero-based formulas.

### Task 3: Rewrite the bundled configuration and DynaPin documentation

**Files:**
- Modify: `resources/profiles/Kingroon/dynapin/kp3s.json:2-16`
- Modify: `resources/profiles/Kingroon/dynapin/README.md:1-190`
- Modify: `docs/dynapin-support-optimization-plan.ja.md:16`
- Modify: `docs/dynapin-support-optimization-plan.en.md:16`
- Modify: `docs/dynapin-prepare-selection-plan.ja.md:59-140`

- [x] **Step 1: Update the KP3S JSON schema**

Replace the existing `grid.origin` and `grid.physical_origin` objects with:

```json
"pull_origin": {
  "y": "14",
  "z": "5.0"
},
"support_origin": {
  "y": "18.0",
  "z": "4.0"
}
```

Remove `row` and `col` from the JSON entirely. Preserve row/column counts, pitch, support exclusion, and pull-G-code values.

- [x] **Step 2: Rewrite the README formulas and schema table**

Document `pull_origin.y/z` as the pull-G-code base and `support_origin.y/z` as the installed pin-array/support-geometry base. State that row and column indices always start at zero. Replace formulas with:

```text
pull_y = pull_origin.y + col × pitch.col_y + pull_gcode.y_offset
pull_z = pull_origin.z + row × pitch.row_z
support_pin_y = support_origin.y + col × pitch.col_y
support_pin_z = support_origin.z + row × pitch.row_z
```

Remove the old-origin migration section and all references to `origin.row`, `origin.col`, and configurable origin indices. Keep the selected-pin syntax as `row,col`.

- [x] **Step 3: Update planning documents that describe the old schema**

Change the support optimization plans to describe `pull_origin` and `support_origin` rather than a generic origin row/column/Y/Z block. Update the Japanese prepare-selection plan’s reference JSON and current-implementation notes so they no longer claim that the old `grid.origin` schema is the active or target shape.

- [x] **Step 4: Check documentation for stale schema names**

Run:

```bash
rg -n -S 'origin_row|origin_col|physical_origin|grid\.origin|origin\.row|origin\.col' \
  resources/profiles/Kingroon/dynapin docs/dynapin-*.md src/libslic3r/DynaPin.* tests/libslic3r/test_dynapin_preview.cpp
```

Expected: no stale runtime/configuration references remain. The design spec may still mention removed names as part of its migration scope.

### Task 4: Verify runtime behavior and the intentional schema break

**Files:**
- Verify: `src/libslic3r/DynaPin.cpp`
- Verify: `tests/libslic3r/test_dynapin_preview.cpp`
- Verify: `tests/fff_print/test_print.cpp`
- Verify: `resources/profiles/Kingroon/dynapin/kp3s.json`

- [x] **Step 1: Run focused DynaPin tests**

Run:

```bash
ctest --test-dir build --output-on-failure -R 'DynaPin|dynapin'
```

Expected: all matching DynaPin preview, coordinate, selection, collision, and support tests pass using the renamed bundled configuration.

- [ ] **Step 2: Run the complete test target**

Not run: the configured macOS build tree has no `tests` build target, and the full CTest suite was not rerun after the scoped verification. The 19 matching DynaPin tests were run successfully.

Run:

```bash
cmake --build build --target tests --parallel
ctest --test-dir build --output-on-failure
```

Expected: the build and complete test suite pass, or any unrelated pre-existing failures are recorded with their test names and output.

- [x] **Step 3: Perform a source-level old-name check**

Run:

```bash
rg -n -S 'origin_row|origin_col|physical_origin_y|physical_origin_z|config\.origin_y|config\.origin_z' \
  src/libslic3r/DynaPin.* tests/libslic3r/test_dynapin_preview.cpp resources/profiles/Kingroon/dynapin docs/dynapin-*.md
git diff --check
```

Expected: the source/configuration search returns no old runtime names, and `git diff --check` reports no whitespace errors.

- [x] **Step 4: Review the final behavior against the design**

Confirm that:

```text
pin_y = support_origin_y + col * col_pitch_y
pin_z = support_origin_z + row * row_pitch_z
pull_y = pull_origin_y + col * col_pitch_y + y_offset
pull_z = pull_origin_z + row * row_pitch_z
```

and that `support_block_y_offset`, `approach_y_offset`, `z_offset`, selected-pin syntax, preview comments, and support projection termination behavior are unchanged.
