# DynaPin Support Z Origin and Blocker Naming Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use [ ] syntax and must be completed task-by-task.

**Goal:** Make support_origin.z the per-column blocker-top origin, remove the relative z_above/pin_z concepts, and use consistent blocker dimension names.

**Architecture:** DynaPin will expose one BlockerZRange calculation per (row, col) pin. The range is z_max = support_origin.z + col * col_pitch_z and z_min = z_max - blocker_height_z; blocker geometry, virtual landing surfaces, preview boxes, sorting, and G-code scheduling consume this range. SupportMaterial.cpp continues to consume computed z_min, z_max, and surface.print_z.

**Tech Stack:** C++17, nlohmann::json, Catch2, CMake/CTest, Markdown, JSON printer profiles.

---

## File map

- Modify src/libslic3r/DynaPin.hpp: add BlockerZRange, rename the height field, expose blocker_z_range(), and remove pin_z().
- Modify src/libslic3r/DynaPin.cpp: parse new keys and route every per-pin Z calculation through blocker_z_range().
- Modify src/libslic3r/GCode.cpp:5631-5639: schedule pulls from the range top.
- Modify src/libslic3r/Support/SupportMaterial.cpp:768-770: replace the direct pin_z() diagnostic.
- Modify resources/profiles/Kingroon/dynapin/kp3s.json: use top-origin Z and renamed blocker dimensions.
- Modify resources/profiles/Kingroon/dynapin/README.md: document formulas and the schema change.
- Modify AGENTS.md: update the DynaPin blocker invariant.
- Modify tests/libslic3r/test_dynapin_preview.cpp: test the range API and preserve pull-coordinate coverage.
- Modify tests/fff_print/test_print.cpp: update the KP3S lower/upper range expectation.
- Modify tests/fff_print/test_support_material.cpp: update pin-top heights and assert range-relative behavior.

### Task 1: Add the range API regression test

**Files:**
- Modify: tests/libslic3r/test_dynapin_preview.cpp:81-103
- Modify: src/libslic3r/DynaPin.hpp

- [ ] **Step 1: Write the failing range test**

Replace the direct pin_z() assertion in the existing “DynaPin support and pull coordinates are independent” test with:

~~~cpp
    config.support_origin_z    = 7.55;
    config.col_pitch_z         = 7.4;
    config.blocker_height_z    = 5.;

    const DynaPin::Pin pin{1, 1};
    const DynaPin::BlockerZRange range = DynaPin::blocker_z_range(config, pin);
    CHECK(range.z_max == Catch::Approx(14.95));
    CHECK(range.z_min == Catch::Approx(9.95));
~~~

Keep the existing pull-origin values and G-code assertions unchanged.

- [ ] **Step 2: Run the focused test build and confirm the new API fails**

Run:

~~~bash
cmake --build build/arm64 --target libslic3r_tests --parallel
~~~

Expected: compilation fails because the new range type, field, and function are not yet defined, and because old production references still exist.

- [ ] **Step 3: Define the public range contract**

In DynaPin.hpp, replace:

~~~cpp
    double blocker_width_y  = 0.;
    double blocker_z_max    = 0.;
    double pin_z_height     = 0.;
~~~

with:

~~~cpp
    double blocker_width_y  = 0.;
    double blocker_height_z = 0.;
~~~

Add before LocalBlocker:

~~~cpp
struct BlockerZRange
{
    double z_min = 0.;
    double z_max = 0.;
};

BlockerZRange blocker_z_range(const Config& config, const Pin& pin);
~~~

Remove the declaration of double pin_z(const Config&, const Pin&).

### Task 2: Implement the new DynaPin geometry contract

**Files:**
- Modify: src/libslic3r/DynaPin.cpp:60-64,114-125,273-280,312-321,325-369,517-533

- [ ] **Step 1: Implement the single range calculation**

Replace pin_z() with:

~~~cpp
BlockerZRange blocker_z_range(const Config& config, const Pin& pin)
{
    const double z_max = config.support_origin_z + double(pin.col) * config.col_pitch_z;
    return { z_max - config.blocker_height_z, z_max };
}
~~~

Here support_origin.z is the blocker top origin for column zero.

- [ ] **Step 2: Parse only the new JSON keys**

Replace the current support-exclusion parsing with:

~~~cpp
    config.blocker_width_y  = number_or(exclusion, "blocker_width_y", config.blocker_width_y);
    config.blocker_height_z = number_or(exclusion, "blocker_height_z", config.blocker_height_z);
~~~

Do not keep compatibility aliases for width_y, y_width, z_above, z_max, z_range, or pin_z_height.

- [ ] **Step 3: Use BlockerZRange in all geometry**

In blocker_for_pin_shift(), surface_for_pin_shift(), and selected_blocker_boxes(), replace every local pin_z, blocker_z_max, and pin_z_height calculation with:

~~~cpp
const BlockerZRange z_range = blocker_z_range(config, pin);
~~~

Use z_range.z_min and z_range.z_max for blocker bounds, set VirtualSupportSurface::print_z to z_range.z_max, and set BlockerBox::pin_pos.z to z_range.z_max. Keep existing XY geometry and instance-shift handling unchanged.

- [ ] **Step 4: Update sorting and diagnostics**

Sort pins using blocker_z_range(config, pin).z_max. Update configuration and candidate logs to report computed z_min/z_max instead of reconstructing them from removed fields.

- [ ] **Step 5: Build and run the focused library tests**

Run:

~~~bash
cmake --build build/arm64 --target libslic3r_tests --parallel
build/arm64/tests/libslic3r/libslic3r_tests "[DynaPin]"
~~~

Expected: the target builds and all DynaPin tests pass.

### Task 3: Update G-code scheduling and support diagnostics

**Files:**
- Modify: src/libslic3r/GCode.cpp:5631-5639
- Modify: src/libslic3r/Support/SupportMaterial.cpp:768-770

- [ ] **Step 1: Use the blocker top for pull scheduling**

Replace:

~~~cpp
const double pin_z = DynaPin::pin_z(dynapin_config, pin);
if (pin_z + dynapin_config.blocker_z_max <= print_z + EPSILON) {
~~~

with:

~~~cpp
const DynaPin::BlockerZRange z_range = DynaPin::blocker_z_range(dynapin_config, pin);
if (z_range.z_max <= print_z + EPSILON) {
~~~

Do not change pull_gcode_for_pin(); pull motion still uses pull_origin.

- [ ] **Step 2: Remove the direct SupportMaterial pin-Z diagnostic**

Compute BlockerZRange once for the candidate and log support_z=[z_min,z_max] plus surface_z. Keep the support algorithm consuming LocalBlocker::z_min/z_max and VirtualSupportSurface::print_z.

- [ ] **Step 3: Verify no production API references remain**

Run:

~~~bash
rg -n -S "DynaPin::pin_z|blocker_z_max|pin_z_height|z_above" src tests resources/profiles/Kingroon/dynapin AGENTS.md
~~~

Expected: no production or test references remain.

### Task 4: Migrate the bundled profile and documentation

**Files:**
- Modify: resources/profiles/Kingroon/dynapin/kp3s.json:9-21
- Modify: resources/profiles/Kingroon/dynapin/README.md:49-73,89-152,218-227
- Modify: AGENTS.md:36-39

- [ ] **Step 1: Update the KP3S profile**

Use:

~~~json
"support_origin": {
  "y": "18.0",
  "z": "7.55"
},
...
"support_exclusion": {
  "blocker_width_y": 12.4,
  "blocker_height_z": 5
}
~~~

For pin 4,4 this yields z_min=32.15 and z_max=37.15.

- [ ] **Step 2: Rewrite the README**

Document:

~~~text
blocker_z_max = support_origin.z + col × pitch.col_z
blocker_z_min = blocker_z_max - blocker_height_z
~~~

State that support_origin.z is the blocker-top origin for column zero, blocker_height_z extends downward, the virtual support surface is at blocker_z_max, and pull coordinates remain controlled by pull_origin. Replace all removed-name examples and schema entries.

- [ ] **Step 3: Align AGENTS.md with the new safety invariant**

State that each blocker covers only its computed [z_min,z_max] span, where z_min = z_max - blocker_height_z; never extend an upper pin blocker to z=0.

### Task 5: Update integration tests

**Files:**
- Modify: tests/fff_print/test_print.cpp:499-504
- Modify: tests/fff_print/test_support_material.cpp:178-196,235-244,296-309

- [ ] **Step 1: Update pin (0,1) expectations**

With the new KP3S profile, pin (0,1) has z_min=9.95 and z_max=14.95. Replace the old lower-bound expectation 6.4 with 9.95 and add an upper-bound assertion for 14.95.

- [ ] **Step 2: Update pin (0,5) surface heights**

Pin (0,5) has z_min=39.55 and z_max=44.55. Replace the first support-material scenario's old surface-related expectations 43.15, 43.5, and 43.2 with 44.55, 44.9, and 44.6.

- [ ] **Step 3: Keep stacked and propagated tests range-relative**

Retain their use of blocker.z_min and blocker.z_max, and add CHECK(blocker.z_max > blocker.z_min) for each selected blocker. Do not add old absolute heights.

- [ ] **Step 4: Build and run FFF DynaPin tests**

Run:

~~~bash
cmake --build build/arm64 --target fff_print_tests --parallel
build/arm64/tests/fff_print/fff_print_tests "[DynaPin]"
~~~

Expected: all matching print, support-material, stacked-pin, collision, and copy-independence tests pass.

### Task 6: Format and verify the complete change

**Files:**
- Verify: all files listed above.

- [ ] **Step 1: Format changed C++**

Run:

~~~bash
clang-format -i src/libslic3r/DynaPin.hpp src/libslic3r/DynaPin.cpp src/libslic3r/GCode.cpp src/libslic3r/Support/SupportMaterial.cpp tests/libslic3r/test_dynapin_preview.cpp tests/fff_print/test_print.cpp tests/fff_print/test_support_material.cpp
~~~

- [ ] **Step 2: Run focused CTest**

Run:

~~~bash
ctest --test-dir build/arm64 --output-on-failure -R 'DynaPin|fff_print'
~~~

Expected: all matching tests pass.

- [ ] **Step 3: Build the application**

Run:

~~~bash
cmake --build build/arm64 --target OrcaSlicer --parallel
~~~

Expected: the application builds with no removed API or configuration references.

- [ ] **Step 4: Review the final diff**

Run:

~~~bash
git diff --check
git diff --stat
git status --short
~~~

Confirm that pre-existing untracked .claude/skills/ and models/ are not added.

- [ ] **Step 5: Commit the implementation**

~~~bash
git add AGENTS.md src/libslic3r/DynaPin.hpp src/libslic3r/DynaPin.cpp src/libslic3r/GCode.cpp src/libslic3r/Support/SupportMaterial.cpp resources/profiles/Kingroon/dynapin/kp3s.json resources/profiles/Kingroon/dynapin/README.md tests/libslic3r/test_dynapin_preview.cpp tests/fff_print/test_print.cpp tests/fff_print/test_support_material.cpp docs/superpowers/plans/2026-08-27-dynapin-support-z-naming.md
git commit -m "refactor: clarify DynaPin support blocker coordinates"
~~~
