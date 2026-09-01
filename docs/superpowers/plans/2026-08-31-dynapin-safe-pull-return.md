# DynaPin Safe Pull and Return Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift vertically before DynaPin movement, run all pulls due at a layer, and return to the writer-known lifted XYZ before normal printing resumes.

**Architecture:** `GCode::change_layer()` owns the safety envelope because it has the layer-change and writer state. `DynaPin` remains responsible for machine-coordinate movement formatting and gains a small return-G-code formatter. Raw DynaPin motion ends exactly at the XYZ already retained by `GCodeWriter`, avoiding manual mutation of private lift bookkeeping.

**Tech Stack:** C++17, OrcaSlicer `GCode`/`GCodeWriter`, DynaPin G-code generation, Catch2, CMake.

---

## File Structure

- Modify `src/libslic3r/DynaPin.hpp`: declare the return-G-code formatter.
- Modify `src/libslic3r/DynaPin.cpp`: format XY-at-clearance followed by Z restoration.
- Modify `src/libslic3r/GCode.cpp`: collect due pins and wrap them in one immediate-lift/return envelope.
- Modify `tests/libslic3r/test_dynapin_preview.cpp`: unit-test return ordering and formatting.
- Modify `tests/fff_print/test_gcode.cpp`: slice a real print and verify lift/pull/return ordering for multiple pins.

### Task 1: Format the DynaPin return path

**Files:**
- Modify: `tests/libslic3r/test_dynapin_preview.cpp`
- Modify: `src/libslic3r/DynaPin.hpp`
- Modify: `src/libslic3r/DynaPin.cpp`

- [ ] **Step 1: Write the failing return-path test**

Add this test beside the existing pull-G-code contract test:

```cpp
TEST_CASE("DynaPin return keeps clearance until XY is restored", "[DynaPin]")
{
    DynaPin::Config config;
    config.pull_gcode.travel_feedrate = 5000.;
    config.pull_gcode.pull_feedrate   = 1500.;

    const std::string gcode = DynaPin::return_gcode(config, Vec3d(102.667, 97.188, 59.3));

    CHECK(gcode == "; BEGIN_DYNAPIN_RETURN\n"
                   "G1 X102.6670 Y97.1880 F5000.0000\n"
                   "G1 Z59.3000 F1500.0000\n"
                   "; END_DYNAPIN_RETURN\n");
}
```

- [ ] **Step 2: Build and run the focused test to verify RED**

Run:

```bash
cmake --build build/arm64 --target libslic3r_tests --config Debug --parallel
build/arm64/tests/libslic3r/Debug/libslic3r_tests.app/Contents/MacOS/libslic3r_tests "DynaPin return keeps clearance until XY is restored"
```

Expected: compilation fails because `DynaPin::return_gcode` is not declared.

- [ ] **Step 3: Declare the public formatter**

Add to `src/libslic3r/DynaPin.hpp` beside `pull_gcode_for_pin`:

```cpp
std::string return_gcode(const Config& config, const Vec3d& return_position);
```

- [ ] **Step 4: Implement the minimal formatter**

Add to `src/libslic3r/DynaPin.cpp` after `pull_gcode_for_pin`:

```cpp
std::string return_gcode(const Config& config, const Vec3d& return_position)
{
    std::ostringstream gcode;
    gcode << std::fixed << std::setprecision(4);
    gcode << "; BEGIN_DYNAPIN_RETURN\n";
    gcode << "G1 X" << return_position.x() << " Y" << return_position.y()
          << " F" << config.pull_gcode.travel_feedrate << "\n";
    gcode << "G1 Z" << return_position.z()
          << " F" << config.pull_gcode.pull_feedrate << "\n";
    gcode << "; END_DYNAPIN_RETURN\n";
    return gcode.str();
}
```

- [ ] **Step 5: Rebuild and verify GREEN**

Run the focused command from Step 2.

Expected: one Catch2 test passes.

- [ ] **Step 6: Commit the formatter slice**

```bash
git add src/libslic3r/DynaPin.hpp src/libslic3r/DynaPin.cpp tests/libslic3r/test_dynapin_preview.cpp
git commit -m "Add DynaPin safe return path"
```

### Task 2: Wrap due pulls in one safe movement envelope

**Files:**
- Modify: `tests/fff_print/test_gcode.cpp`
- Modify: `src/libslic3r/GCode.cpp:5602-5642`

- [ ] **Step 1: Add test helpers for a real DynaPin slice**

Add the required headers and helpers to `tests/fff_print/test_gcode.cpp`:

```cpp
#include "libslic3r/Print.hpp"
#include "libslic3r/Utils.hpp"
#include "test_data.hpp"

#include <boost/filesystem/path.hpp>

namespace {

class ResourcesDirGuard
{
public:
    explicit ResourcesDirGuard(const std::string& path) : m_previous(resources_dir()) { set_resources_dir(path); }
    ~ResourcesDirGuard() { set_resources_dir(m_previous); }

private:
    std::string m_previous;
};

std::string test_resources_dir()
{
    return (boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() / "resources").string();
}

size_t count_occurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}

} // namespace
```

- [ ] **Step 2: Write the failing integration test**

Add this test to `tests/fff_print/test_gcode.cpp`:

```cpp
TEST_CASE("DynaPin pulls share one immediate lift and safe return", "[GCode][DynaPin]")
{
    ResourcesDirGuard resources_guard(test_resources_dir());
    Print print;
    Model model;
    Test::init_print({TestMesh::cube_20x20x20}, print, model, {
        {"enable_dynapin_support_optimization", true},
        {"dynapin_config_path", "Kingroon/dynapin/kp3s.json"},
        {"dynapin_selected_pins", "0,0 0,1"},
        {"layer_height", 0.3},
        {"first_layer_height", 0.3},
        {"retract_when_changing_layer", true},
        {"retract_lift_enforce", "All Surfaces"},
        {"z_hop", "0.2"},
        {"z_hop_types", "Auto Lift"},
        {"gcode_comments", true},
        {"start_gcode", ""},
        {"printable_area", "0x0,180x0,180x180,0x180"},
    });

    const std::string gcode       = Test::gcode(print);
    const size_t first_pull       = gcode.find("; BEGIN_DYNAPIN_PULL ROW=0 COL=0");
    const size_t second_pull      = gcode.find("; BEGIN_DYNAPIN_PULL ROW=0 COL=1");
    const size_t immediate_lift   = gcode.rfind("normal lift Z", first_pull);
    const size_t last_pull_end    = gcode.find("; END_DYNAPIN_PULL", second_pull);
    const size_t return_begin     = gcode.find("; BEGIN_DYNAPIN_RETURN", last_pull_end);
    const size_t return_end       = gcode.find("; END_DYNAPIN_RETURN", return_begin);

    REQUIRE(first_pull != std::string::npos);
    REQUIRE(second_pull != std::string::npos);
    REQUIRE(immediate_lift != std::string::npos);
    REQUIRE(last_pull_end != std::string::npos);
    REQUIRE(return_begin != std::string::npos);
    REQUIRE(return_end != std::string::npos);
    CHECK(immediate_lift < first_pull);
    CHECK(first_pull < second_pull);
    CHECK(second_pull < last_pull_end);
    CHECK(last_pull_end < return_begin);
    CHECK(return_begin < return_end);
    CHECK(count_occurrences(gcode, "; BEGIN_DYNAPIN_PULL ROW=0") == 2);
    CHECK(count_occurrences(gcode, "; BEGIN_DYNAPIN_RETURN") == 1);
}
```

- [ ] **Step 3: Build and run the integration test to verify RED**

Run:

```bash
cmake --build build/arm64 --target fff_print_tests --config Debug --parallel
build/arm64/tests/fff_print/Debug/fff_print_tests.app/Contents/MacOS/fff_print_tests "DynaPin pulls share one immediate lift and safe return"
```

Expected: the test fails because no immediate normal lift or DynaPin return block surrounds the pulls.

- [ ] **Step 4: Collect due pins without emitting them immediately**

Replace the existing DynaPin loop in `GCode::change_layer()` with collection logic:

```cpp
        std::vector<std::pair<std::string, DynaPin::Pin>> due_pins;
        for (const DynaPin::Pin& pin : DynaPin::resolved_pins(*m_print)) {
            const DynaPin::BlockerZRange range = DynaPin::blocker_z_range(dynapin_config, pin);
            const std::string key = std::to_string(pin.row) + ":" + std::to_string(pin.col);
            if (range.z_max <= print_z + EPSILON && m_dynapin_pulls_done.find(key) == m_dynapin_pulls_done.end())
                due_pins.emplace_back(key, pin);
        }
```

- [ ] **Step 5: Emit one immediate vertical lift, all pulls, and one return**

Append this immediately after collection, still inside the loaded DynaPin configuration block:

```cpp
        if (!due_pins.empty()) {
            gcode += m_writer.eager_lift(LiftType::NormalLift);
            Vec3d return_position = m_writer.get_position();
            const Vec2f xy_offset = m_writer.get_xy_offset();
            return_position.x() -= xy_offset.x();
            return_position.y() -= xy_offset.y();

            for (const auto& [key, pin] : due_pins) {
                gcode += DynaPin::pull_gcode_for_pin(dynapin_config, pin);
                m_dynapin_pulls_done.insert(key);
            }
            gcode += DynaPin::return_gcode(dynapin_config, return_position);
        }
```

Do not call `GCodeWriter::set_position()`: the raw DynaPin sequence physically returns to the position the writer already retains, including its active `m_lifted` state.

- [ ] **Step 6: Rebuild and verify GREEN**

Run the focused integration test command from Step 3.

Expected: one Catch2 test passes, with two pull blocks inside one lift/return envelope.

- [ ] **Step 7: Run all focused DynaPin tests**

```bash
build/arm64/tests/libslic3r/Debug/libslic3r_tests.app/Contents/MacOS/libslic3r_tests "[DynaPin]"
build/arm64/tests/fff_print/Debug/fff_print_tests.app/Contents/MacOS/fff_print_tests "[DynaPin]"
```

Expected: all DynaPin-tagged tests pass.

- [ ] **Step 8: Commit the integration slice**

```bash
git add src/libslic3r/GCode.cpp tests/fff_print/test_gcode.cpp
git commit -m "Safely restore position after DynaPin pulls"
```

### Task 3: Verify the Gymnast regression output

**Files:**
- Read: `models/RemoveGymnastBase.3mf`
- Generate outside the repository: `/private/tmp/orca-dynapin-safe-return/`

- [ ] **Step 1: Build the updated application**

```bash
cmake --build build/arm64 --target OrcaSlicer --config Debug --parallel
```

Expected: the Debug application builds successfully.

- [ ] **Step 2: Re-slice the Gymnast model into a temporary directory**

```bash
mkdir -p /private/tmp/orca-dynapin-safe-return
build/arm64/src/Debug/OrcaSlicer.app/Contents/MacOS/OrcaSlicer --debug 3 --slice 0 --outputdir /private/tmp/orca-dynapin-safe-return models/RemoveGymnastBase.3mf
```

Expected: a G-code file is produced under `/private/tmp/orca-dynapin-safe-return`.

- [ ] **Step 3: Inspect the first pull envelope**

```bash
rg -n -C 12 "BEGIN_DYNAPIN_PULL ROW=7 COL=8|BEGIN_DYNAPIN_RETURN|END_DYNAPIN_RETURN|normal lift Z" /private/tmp/orca-dynapin-safe-return/*.gcode
```

Expected ordering:

```text
normal lift Z
BEGIN_DYNAPIN_PULL ROW=7 COL=8
END_DYNAPIN_PULL
BEGIN_DYNAPIN_RETURN
G1 X... Y...
G1 Z...
END_DYNAPIN_RETURN
```

- [ ] **Step 4: Confirm the unsafe direct approach is gone**

Use the generated context to verify that the first `G1 Y105.7000` occurs only after the vertical safety lift, and that normal next-layer travel begins only after `END_DYNAPIN_RETURN`.

- [ ] **Step 5: Run formatting and diff checks**

```bash
clang-format -i src/libslic3r/DynaPin.hpp src/libslic3r/DynaPin.cpp src/libslic3r/GCode.cpp tests/libslic3r/test_dynapin_preview.cpp tests/fff_print/test_gcode.cpp
git diff --check
```

Expected: no formatting or whitespace errors.

- [ ] **Step 6: Run the two test executables once more**

```bash
build/arm64/tests/libslic3r/Debug/libslic3r_tests.app/Contents/MacOS/libslic3r_tests "[DynaPin]"
build/arm64/tests/fff_print/Debug/fff_print_tests.app/Contents/MacOS/fff_print_tests "[DynaPin]"
```

Expected: all DynaPin-tagged tests pass after formatting.
