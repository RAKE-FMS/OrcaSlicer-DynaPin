# OrcaSlicer Implementation Guide for AI Coding Agents

> Purpose: Give AI coding agents enough architectural context to modify OrcaSlicer without blindly searching the repository.
>
> Surveyed: 2026-09-24
>
> Primary target: OrcaSlicer `main`
>
> Scope: C++ slicing engine, configuration, support generation, G-code generation, GUI integration, plugins, testing, and implementation workflow.

---

## 0. How an AI agent should use this document

Before editing OrcaSlicer:

1. Read the repository root `AGENTS.md`.
2. Identify which subsystem owns the requested behavior.
3. Trace the existing execution path before adding code.
4. Search for an existing config option, helper, class, or processing step before creating a new abstraction.
5. Keep the default behavior unchanged when the feature is disabled.
6. Check invalidation/recomputation behavior when a slicing setting is added or changed.
7. Add or update targeted tests where practical.
8. Build the smallest relevant target before doing a full application build.

Do not infer architecture only from filenames. OrcaSlicer inherits substantial architecture from Bambu Studio / PrusaSlicer / Slic3r and contains historical naming.

---

# 1. Repository orientation

## Important entry points

| Concern | Primary location | Notes |
|---|---|---|
| Application startup | `src/OrcaSlicer.cpp` | Top-level executable entry |
| Main slicing coordinator | `src/libslic3r/Print.cpp` | `Print`-level pipeline |
| Per-object slicing | `src/libslic3r/PrintObject.cpp` | Object-scoped slicing steps |
| Print settings | `src/libslic3r/PrintConfig.cpp` / `.hpp` | Printer, filament, and process settings |
| Normal support | `src/libslic3r/Support/SupportMaterial.cpp` | Traditional support generation |
| Tree support | `src/libslic3r/Support/TreeSupport.cpp` | Tree/organic support |
| High-level G-code generation | `src/libslic3r/GCode.cpp` / `.hpp` | Converts toolpaths to output sequence |
| Low-level G-code writer | `src/libslic3r/GCodeWriter.cpp` / `.hpp` | Emits machine commands and tracks writer state |
| Layer model | `src/libslic3r/Layer.cpp` / `.hpp` | Sliced layer representation |
| Print regions | `src/libslic3r/PrintRegion.cpp` / `.hpp` | Region-specific print settings |
| Presets | `src/libslic3r/Preset.cpp` / `.hpp` | Preset storage/resolution |
| GUI | `src/slic3r/GUI/` | wxWidgets UI |
| Main plate UI | `src/slic3r/GUI/Plater.cpp` | Plate actions and slicing interaction |
| Settings tabs | `src/slic3r/GUI/Tab.cpp` | GUI settings integration |
| Tests | `tests/` | Catch2-based test suites |
| Printer profiles | `resources/profiles/` | Manufacturer/profile JSON |

Repository-level coding instructions are in:

```text
AGENTS.md
CLAUDE.md
tests/AGENTS.md
```

Always inspect them before making a non-trivial change.

---

# 2. Mental model of OrcaSlicer

The most useful high-level model is:

```text
Model / 3MF / STL
       |
       v
      GUI
  Plater / Tabs
       |
       v
Resolved configuration
 PrintConfig / DynamicPrintConfig / Presets
       |
       v
      Print
   global job state
       |
       +---------------------+
       |                     |
       v                     v
  PrintObject            PrintRegion
 per model/object      shared config region
       |
       v
      Layer
       |
       v
   LayerRegion
       |
       +-----------------------------+
       |          |         |        |
       v          v         v        v
    Slice     Perimeter   Infill   Support
                                   |
                         SupportMaterial /
                            TreeSupport
       |
       v
 Extrusion entities / paths
       |
       v
      GCode
 ordering, travel, retract, tool state
       |
       v
   GCodeWriter
 machine command emission
       |
       v
     .gcode
```

Key design distinction:

- `Print` coordinates the whole print job.
- `PrintObject` owns processing for one printable object.
- `Layer` represents one Z slice.
- `LayerRegion` represents a region of a layer under a specific print configuration.
- Geometry generation should normally happen before G-code serialization.
- `GCodeWriter` is stateful. Directly emitting movement commands without respecting its tracked state can corrupt later G-code.

---

# 3. Slicing call path

The official developer documentation traces slicing from the GUI approximately as:

```text
Slice Plate
  |
  v
Plater::priv::on_action_slice_plate(...)
  |
  v
Plater::reslice()
  |
  v
Plater::priv::restart_background_process(...)
  |
  v
BackgroundSlicingProcess::start()
  |
  v
BackgroundSlicingProcess worker thread
  |
  v
FFF processing
  |
  v
Print::process()
```

After entering the slicing engine, object-level work is primarily executed through `PrintObject`.

Conceptually:

```text
Print::process()
 |
 +-- object slicing
 |    `-- PrintObject::slice()
 |
 +-- perimeter generation
 |    `-- PrintObject::make_perimeters()
 |
 +-- prepare/fill
 |    `-- PrintObject::*infill*()
 |
 +-- support
 |    `-- PrintObject::generate_support_material()
 |
 +-- print-level structures
 |    +-- skirt
 |    +-- brim
 |    `-- wipe tower
 |
 `-- G-code export
      `-- GCode::do_export(...)
```

Exact ordering can evolve. Verify the current `Print::process()` / processing-step implementation before depending on an assumed order.

---

# 4. Processing-step model

OrcaSlicer uses explicit processing steps to track which pieces of derived data are valid.

Object-level steps include concepts such as:

```text
posSlice
posPerimeters
posPrepareInfill
posInfill
posIroning
posSupportMaterial
posSimplifyPath
```

Print-level steps include concepts such as:

```text
psWipeTower
psSkirtBrim
psGCodeExport
```

Why this matters:

When a setting changes, OrcaSlicer should invalidate only the processing stages affected by that setting.

A new slicing option is not complete merely because:

```cpp
config.def("my_option", ...);
```

exists.

You must also determine:

```text
What data depends on this option?
Which processing step must be invalidated when it changes?
Does it affect object geometry, support, G-code only, or the whole print?
```

Search for:

```text
invalidate
invalidate_state_by_config_options
PrintObjectStep
PrintStep
posSupportMaterial
psGCodeExport
```

when implementing a setting.

---

# 5. Configuration system

## Primary files

```text
src/libslic3r/PrintConfig.hpp
src/libslic3r/PrintConfig.cpp
src/libslic3r/Preset.hpp
src/libslic3r/Preset.cpp
src/slic3r/GUI/Tab.cpp
```

## Typical setting lifecycle

A process setting usually travels through several layers:

```text
Definition
   |
   v
PrintConfig.cpp
   |
   v
Config object
   |
   +--> preset/profile serialization
   |
   +--> GUI control
   |
   +--> CLI / 3MF / config parsing
   |
   v
Print / PrintObject / PrintRegion config
   |
   v
slicing algorithm
```

Before adding a new option, search for a similar existing option and trace all of its uses.

Recommended searches:

```bash
rg 'support_threshold_angle' src resources tests
rg 'support_interface_top_layers' src resources tests
rg 'enable_support' src resources tests
```

Use a structurally similar option as the template.

## Common configuration scopes

Be careful about which configuration object owns a value.

Typical concepts include:

- printer-wide configuration
- print/process configuration
- filament configuration
- object-specific configuration
- region-specific configuration

Do not assume every option is accessible through the same `config()` object.

When reading code such as:

```cpp
object.config()
object.print()->config()
region.config()
```

the distinction is intentional.

---

# 6. Core data structures

## `Print`

Use `Print` when a change concerns the whole print job.

Typical responsibilities:

- collection of `PrintObject`s
- global print configuration
- print-wide processing steps
- skirt / brim
- wipe tower
- G-code export coordination

Do not put per-object geometry state in `Print` unless it truly needs print-wide ownership.

---

## `PrintObject`

Use `PrintObject` when a change belongs to one printable model/object.

Typical responsibilities:

```text
mesh -> layers
layers -> perimeters
layers -> infill
layers -> support
```

Important conceptual members include:

```text
layers
support layers
object configuration
reference back to Print
```

For a feature that changes support behavior for each model independently, `PrintObject` is usually the natural integration boundary.

---

## `Layer`

Represents one horizontal layer.

Useful concepts:

```text
print_z
height
slice geometry
LayerRegion collection
```

Do not confuse:

```text
layer index
slice Z
print Z
layer height
```

They are related but not interchangeable.

---

## `LayerRegion`

A single layer can contain multiple regions with different settings/material roles.

This matters when writing code that assumes "one layer = one configuration".

Avoid accessing only the first region unless that assumption is guaranteed.

---

## `PrintRegion`

Represents a group of geometry sharing print settings.

Think:

```text
Model volume(s)
      |
      +-- region A: config A
      `-- region B: config B
```

---

# 7. Geometry and units

OrcaSlicer frequently uses integer-scaled coordinates internally.

Do not mix:

```text
millimeters (double / float)
scaled integer coordinates
layer indices
```

without explicit conversion.

Search existing code for:

```cpp
scale_(...)
unscale(...)
scaled(...)
coord_t
coordf_t
Point
Pointf
Polygon
ExPolygon
ExPolygons
```

Use existing conversion utilities rather than hand-writing scale constants.

Geometry libraries and helpers are concentrated under:

```text
src/libslic3r/Geometry/
src/libslic3r/ClipperUtils.*
```

Typical operations include:

```text
union
intersection
difference
offset
projection
containment
```

Prefer OrcaSlicer's existing wrappers to introducing a second geometry convention.

---

# 8. Support generation

## Main files

Normal support:

```text
src/libslic3r/Support/SupportMaterial.cpp
src/libslic3r/Support/SupportMaterial.hpp
```

Tree support:

```text
src/libslic3r/Support/TreeSupport.cpp
src/libslic3r/Support/TreeSupport.hpp
```

Entry point from `PrintObject`:

```text
PrintObject::generate_support_material()
```

Conceptual flow:

```text
object layers
   |
   v
detect unsupported / overhang areas
   |
   v
construct contact/interface/base regions
   |
   v
propagate / merge support regions through Z
   |
   v
generate support toolpaths
   |
   v
support layers / extrusion entities
```

When changing support, first determine which stage the change belongs to:

```text
A. overhang detection
B. support-contact detection
C. support-region propagation
D. collision/blocking
E. support-interface generation
F. support fill/toolpath generation
G. G-code emission
```

Do not implement a geometry constraint at the G-code stage if it should remove support geometry earlier.

---

# 9. Support blockers and custom support constraints

For any feature that excludes support from selected regions, search current source for:

```bash
rg 'blocker' src/libslic3r src/slic3r
rg 'support.*block' src/libslic3r
rg 'support_enforcer' src/libslic3r src/slic3r
```

Important distinction:

```text
GUI/model modifier
       vs.
derived per-layer blocker geometry
       vs.
support algorithm exclusion
```

A robust implementation typically converts external/user-level definitions into the coordinate system and layer representation expected by support generation, then applies the exclusion before generating final support extrusion paths.

If the exclusion is dynamic by layer/Z, make the Z convention explicit.

---

# 10. G-code architecture

## High-level generator

```text
src/libslic3r/GCode.cpp
src/libslic3r/GCode.hpp
```

Responsibilities include:

- layer ordering
- extrusion ordering
- travel planning
- tool changes
- retraction interaction
- custom code
- output assembly

Main export boundary:

```cpp
GCode::do_export(...)
```

---

## Low-level writer

```text
src/libslic3r/GCodeWriter.cpp
src/libslic3r/GCodeWriter.hpp
```

The writer tracks machine state.

Examples of state that may matter:

```text
XYZ position
active tool
extrusion position
retraction state
lift / Z-hop state
acceleration / speed state
coordinate modes
```

### Critical rule

If custom G-code physically moves the toolhead but OrcaSlicer does not update its internal state to match, subsequent generated G-code may be incorrect.

For custom motion:

1. determine the currently tracked writer state,
2. preserve state that will be temporarily changed,
3. emit motion through existing writer helpers when possible,
4. update internal state consistently,
5. restore the intended logical/physical state before normal generation resumes.

Avoid treating generated G-code as merely concatenated text.

---

# 11. Layer-change and custom-motion safety

Features that inject machine motion between layers are particularly risky.

Potential failure modes:

```text
unexpected lazy Z-hop
stale internal XY position
incorrect retract/unretract
incorrect active extruder state
travel planner assuming the wrong location
restore move occurring at the wrong Z
```

Before injecting a movement sequence, inspect existing implementations of:

```bash
rg 'change_layer' src/libslic3r
rg 'travel_to' src/libslic3r/GCode*
rg 'retract' src/libslic3r/GCode*
rg 'unretract' src/libslic3r/GCode*
rg 'lift' src/libslic3r/GCode*
```

Recommended conceptual pattern:

```text
normal tool state
    |
    v
enter controlled custom-motion sequence
    |
    +-- ensure safe Z
    +-- save required state
    +-- execute custom motion
    +-- restore position/state
    |
    v
resume normal layer G-code
```

The exact implementation must follow the current writer/GCode APIs.

---

# 12. GUI architecture

Primary directory:

```text
src/slic3r/GUI/
```

Important files/classes include:

```text
Plater
Tab
GUI_App
Sidebar
BackgroundSlicingProcess
```

Use GUI code for:

- controls
- events
- visualization
- invoking slicing
- presenting validation/errors

Avoid putting slicing algorithms in GUI classes.

Expected separation:

```text
GUI
  |
  v
configuration / model state
  |
  v
libslic3r
```

not:

```text
GUI callback
  |
  `-- complex geometry algorithm
```

---

# 13. From "Slice Plate" to engine

Use this path when debugging "the UI option changed, but slicing did not":

```text
Plater event
  |
  v
Plater::reslice()
  |
  v
BackgroundSlicingProcess
  |
  v
Print data/config synchronization
  |
  v
Print::process()
```

Questions to ask:

```text
Was the config value actually transferred into the Print?
Was the affected step invalidated?
Did background slicing restart?
Is the algorithm reading the object-level or global config?
```

---

# 14. Plugins

As of 2026, OrcaSlicer includes an experimental Python plugin system.

A slicing-pipeline plugin can run at defined pipeline steps and inspect/modify the live slicing context.

Relevant concepts include:

```text
SlicingPipelineCapabilityBase
ctx.step
ctx.print
ctx.object
ctx.config_value(...)
psGCodePostProcess
```

Geometry steps expose live slicing objects during plugin execution.

Post-processing exposes the G-code path instead.

Use a plugin when:

- experimenting without maintaining a large fork
- inspecting the slicing graph
- doing post-processing
- implementing a feature that fits an exposed hook

Modify the C++ engine when:

- the required data is not exposed by plugins
- support/perimeter/infill internals must change
- new persistent slicing data structures are required
- performance-critical integration is needed
- behavior must be a first-class OrcaSlicer feature

Do not assume the plugin API is as stable as the core C++ interfaces; it is explicitly experimental.

---

# 15. Build and tests

Root `AGENTS.md` documents typical builds.

Examples:

```bash
# macOS
cmake --build build/arm64 --config RelWithDebInfo --target all --

# Linux
cmake --build build --config RelWithDebInfo --target all --

# Tests
cd build
ctest --output-on-failure

ctest --test-dir ./tests/libslic3r
ctest --test-dir ./tests/fff_print
```

Before adding a test, read:

```text
tests/AGENTS.md
```

Prefer a targeted test near the algorithm being changed.

For slicing features, useful verification often includes:

```text
unit/geometry test
+
slicing regression test
+
generated G-code inspection
```

---

# 16. AI search strategy

Do not ask the model to read the entire repository first.

Use a layered search.

## Phase 1: identify subsystem

Examples:

```bash
rg 'generate_support_material' src/libslic3r
rg 'do_export' src/libslic3r
rg 'PrintObjectStep' src/libslic3r
```

## Phase 2: follow call sites

```bash
rg 'generate_support_material\(' src tests
```

## Phase 3: trace data ownership

Search the type and config key:

```bash
rg 'support_interface_top_layers' src
rg 'class PrintObject' src/libslic3r
```

## Phase 4: inspect tests and history

```bash
rg 'support' tests
git log -S'support_interface_top_layers' -- src/
git blame <file>
```

History is valuable because architectural intent is often clearer in the introducing PR/commit than in the final code.

---

# 17. Change-location decision table

| Requested change | Start reading here |
|---|---|
| Add a process setting | `PrintConfig.cpp`, similar option, `Tab.cpp` |
| Add printer setting | `PrintConfig.*`, presets/profiles, GUI tabs |
| Change object slicing geometry | `PrintObject.cpp`, slicing/geometry files |
| Change perimeter behavior | `PrintObject.cpp`, perimeter/Arachne code |
| Add infill algorithm | `src/libslic3r/Fill/` |
| Change normal support | `Support/SupportMaterial.cpp` |
| Change tree support | `Support/TreeSupport.cpp` |
| Add support exclusion | blocker/enforcer paths + `SupportMaterial` |
| Inject custom G-code | `GCode.cpp`, `GCodeWriter.*` |
| Change layer ordering | `GCode.cpp` |
| Add GUI button/tool | `src/slic3r/GUI/` |
| Add profile | `resources/profiles/` |
| Add CLI-visible setting | trace similar `PrintConfig` option |
| Add slicing hook without fork | plugin slicing pipeline |
| Diagnose stale slice result | processing-step invalidation |

---

# 18. Common mistakes for AI-generated patches

## Mistake 1: adding a config option but not invalidating the slice

Symptom:

```text
UI value changes, but reslicing reuses stale data.
```

Check processing-step invalidation.

---

## Mistake 2: putting algorithmic behavior in GUI code

Move slicing logic into `libslic3r`.

---

## Mistake 3: duplicating a helper

Search the repository before adding:

```text
geometry utility
config lookup helper
filament/nozzle helper
G-code state helper
```

OrcaSlicer has many historical utilities that are not obvious from filenames.

---

## Mistake 4: incorrect config scope

Example failure:

```text
reading global PrintConfig when the option is object-specific
```

Trace a similar option.

---

## Mistake 5: emitting raw G-code without updating state

This can break subsequent moves, retraction, or layer transitions.

---

## Mistake 6: treating support as a single polygon

Support commonly has multiple semantic layers:

```text
contact
interface
base
bottom interface
extrusion paths
```

Know which representation you are modifying.

---

## Mistake 7: mixing scaled and unscaled coordinates

Look at argument and container types before geometry operations.

---

## Mistake 8: changing default behavior

A feature controlled by a new option should normally preserve previous output when disabled.

---

## Mistake 9: editing generated/profile data without compatibility consideration

OrcaSlicer's root instructions explicitly require backward compatibility for:

```text
.3mf project files
printer profiles
format/profile migrations
```

---

# 19. Recommended prompt for an AI coding agent

Use this template when asking an AI to implement a feature:

```text
You are modifying OrcaSlicer.

Read:
1. repository-root AGENTS.md
2. ORCASLICER_IMPLEMENTATION_GUIDE.md
3. tests/AGENTS.md if tests are changed

Task:
<describe behavior>

Before editing:
- identify the owning subsystem
- trace the current call path
- identify config scope
- identify required processing-step invalidation
- find the closest existing analogous implementation
- list the files that need changes and why

Constraints:
- preserve existing behavior when the feature is disabled
- reuse existing helpers
- respect scaled/unscaled coordinate conventions
- do not inject raw G-code without maintaining writer state
- keep GUI and slicing logic separated
- add targeted tests or give a concrete verification procedure

After editing, report:
- changed files
- architectural reason for each change
- processing step(s) affected
- test/build commands run
- known risks or unverified assumptions
```

---

# 20. Prompt for debugging an existing implementation

```text
Trace this OrcaSlicer behavior from its public entry point to the final generated data.

Do not patch immediately.

First provide:
1. call graph
2. data ownership at each stage
3. config values used
4. processing-step invalidation involved
5. state mutations
6. likely failure point

Then propose the smallest fix.

When investigating G-code, distinguish:
- logical slicer position/state
- physical emitted machine moves
- retract state
- Z-hop/lift state

When investigating support, distinguish:
- input object geometry
- overhang/contact geometry
- support-region propagation
- support layer surfaces
- support extrusion paths
```

---

# 21. DynaPin-style extension map

For an extension that dynamically suppresses support in selected spatial regions and performs machine motion to deploy/retract a physical support mechanism, keep the concerns separate.

Recommended separation:

```text
DynaPin configuration / input
        |
        v
resolve pin definitions
        |
        v
convert pin state into layer-local blocker geometry
        |
        v
SupportMaterial
exclude normal/tree support where physical support exists
        |
        v
ordinary support toolpaths
        |
        +----------------------------------+
        |
        v
GCode phase
schedule physical pin actuation
        |
        v
GCodeWriter-safe movement sequence
```

Avoid:

```text
SupportMaterial.cpp directly writing G-code
```

and avoid:

```text
GCode.cpp reconstructing support geometry from scratch
```

The support planner should decide **where support exists**.

The G-code generator should decide **when/how machine motion is emitted**.

A compact DynaPin module boundary could be:

```text
DynaPin.hpp / DynaPin.cpp
    |
    +-- parse/validate config
    +-- map pins to layer/Z geometry
    +-- expose support-blocking regions
    `-- expose actuation schedule

SupportMaterial
    `-- consume blocking geometry

GCode
    `-- consume actuation schedule
```

This keeps geometry decisions reusable and machine-state code localized.

---

# 22. DynaPin review checklist

When reviewing a DynaPin change, verify:

```text
[ ] Pin coordinates are defined in one documented coordinate convention.
[ ] mm vs scaled coordinates are explicit.
[ ] Layer-index vs print-Z conversions are explicit.
[ ] Support blocker generation is deterministic.
[ ] Both normal and tree support behavior are intentionally handled.
[ ] Disabled DynaPin produces baseline OrcaSlicer behavior.
[ ] Pull/retract G-code cannot collide with the model.
[ ] Required Z-hop is immediate rather than deferred/lazy.
[ ] Writer position after custom motion matches physical position.
[ ] Retraction state is preserved.
[ ] Active extruder/tool state is preserved.
[ ] Layer transition resumes from the expected Z.
[ ] Config changes invalidate support and/or G-code as required.
[ ] CLI slicing behaves consistently with GUI slicing.
[ ] A small deterministic 3MF/model can regression-test the behavior.
```

---

# 23. Useful repository searches

## Slicing

```bash
rg 'void Print::process|Print::process\(' src/libslic3r
rg 'PrintObject::slice' src/libslic3r
rg 'PrintObject::make_perimeters' src/libslic3r
rg 'generate_support_material' src/libslic3r
```

## Processing/invalidation

```bash
rg 'PrintObjectStep|PrintStep' src/libslic3r
rg 'invalidate' src/libslic3r/Print*
```

## Support

```bash
rg 'SupportMaterial' src/libslic3r
rg 'TreeSupport' src/libslic3r
rg 'blocker|enforcer' src/libslic3r src/slic3r
```

## G-code

```bash
rg 'do_export' src/libslic3r
rg 'change_layer' src/libslic3r
rg 'GCodeWriter' src/libslic3r
rg 'retract|unretract|lift' src/libslic3r/GCode*
```

## Configuration

```bash
rg 'config.def\(' src/libslic3r/PrintConfig.cpp
rg '<option_name>' src resources tests
```

## GUI

```bash
rg '<option_name>' src/slic3r/GUI
rg 'reslice' src/slic3r/GUI
rg 'BackgroundSlicingProcess' src/slic3r/GUI
```

---

# 24. Source-quality hierarchy

For architectural decisions, use sources in this order:

1. current OrcaSlicer source code
2. repository `AGENTS.md`
3. OrcaSlicer official developer wiki
4. tests
5. Git history / merged PRs
6. upstream Bambu Studio / PrusaSlicer implementation
7. DeepWiki or other generated explanations

Generated documentation is useful for navigation but must not override the current source.

---

# 25. Primary reference sources

## OrcaSlicer repository

https://github.com/OrcaSlicer/OrcaSlicer

## Repository AI/developer instructions

https://github.com/OrcaSlicer/OrcaSlicer/blob/main/AGENTS.md

## Official wiki repository

https://github.com/OrcaSlicer/OrcaSlicer_WIKI

## Official slicing call hierarchy

https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/slicing_hierarchy.md

## Official developer wiki index

https://www.orcaslicer.com/wiki/

## Official plugin slicing-pipeline documentation

https://github.com/OrcaSlicer/OrcaSlicer/wiki/slicing

## DeepWiki slicing-engine overview

https://deepwiki.com/SoftFever/OrcaSlicer/4-slicing-system

Use DeepWiki as a navigation aid, not as authoritative source code documentation.

---

# 26. Minimal context packet for AI agents

If context space is limited, provide the AI with:

```text
AGENTS.md
ORCASLICER_IMPLEMENTATION_GUIDE.md
the .hpp/.cpp files directly involved
one analogous existing implementation
relevant tests
```

For support work:

```text
Print.hpp
Print.cpp
PrintObject.cpp
Support/SupportMaterial.hpp
Support/SupportMaterial.cpp
PrintConfig.hpp
PrintConfig.cpp
```

For custom machine motion:

```text
GCode.hpp
GCode.cpp
GCodeWriter.hpp
GCodeWriter.cpp
Print.cpp
```

For a DynaPin-like feature, give the AI both groups.

---

# 27. Final implementation rule

Before accepting an AI-generated OrcaSlicer patch, be able to answer all five:

```text
1. Which object owns this data?
2. At which slicing step is it created?
3. Which config change invalidates it?
4. In which coordinate/state convention is it represented?
5. Which test proves that disabled/default behavior is unchanged?
```

If any answer is unclear, the patch is not yet architecturally safe.
