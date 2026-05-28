# DynaPin Support Generation Optimization Plan

## Summary

Modify OrcaSlicer itself so selected pin areas are excluded before support generation, instead of editing the generated G-code afterward. Because pin positions vary by 3D printer, the fixed DynaPin configuration will be linked at the `machine` preset level, with the actual configuration stored as a separate JSON file next to the printer profiles.

The primary implementation path is an OrcaSlicer core change. OrcaSlicer does not appear to expose a general plugin API for intervening in support generation, and pre-generation control for both normal and tree supports needs to integrate under `PrintObject::generate_support_material()`.

## Key Changes

- Add printer-specific DynaPin Config files.
  - Example: `resources/profiles/<vendor>/dynapin/<machine-name>.json`
  - Add only `dynapin_config_path` to the machine preset, using a relative path.
  - Use the same key for user machine presets, with user-profile JSON taking priority when available.
- Base the DynaPin Config shape on PyDynaPin.
  - `grid`: origin row/col, origin Y/Z, row pitch Y, col pitch Z
  - `support_exclusion`: Y/X width around the pin center, target Z range or layer range
  - `pull_gcode`: `x_hook`, `x_latch`, `x_front`, offsets, feed rates
- Store slice-time operation settings on the project or object side.
  - `enable_dynapin_support_optimization`
  - Selected pin list as `(row, col)`
  - Resolve the active DynaPin Config automatically from the current machine preset.
- Generate per-layer 2D blocker regions from the selected pins before support generation.
  - Merge normal-support blockers into the existing `SupportAnnotations::blockers_layers` path in `SupportMaterial.cpp`.
  - Merge tree-support blockers into the existing blocker-layers path as well.
  - Remove unwanted support by polygon difference before support contact/intermediate/interface generation, not by deleting G-code lines.
- Insert pull G-code during slicing.
  - Port the equivalent of PyDynaPin's `generate_pull_lines()` to C++.
  - For the initial version, insert immediately after the layer start nearest to `pin_z`.
  - Wrap blocks with `; BEGIN_DYNAPIN_PULL row=... col=...` and `; END_DYNAPIN_PULL ...`.

## Test Plan

- Config resolution:
  - Read the correct JSON from a machine preset's `dynapin_config_path`.
  - Handle missing config, missing files, and invalid JSON with a clear error or by disabling the feature.
- Pin calculation:
  - Verify PyDynaPin-compatible `row,col -> Y,Z` calculation.
  - Detect duplicate and invalid pin selections.
- Support generation:
  - Confirm target-area support polygons/extrusions disappear for both normal and tree supports.
  - Preserve existing behavior when combined with support blockers and support painting.
- G-code:
  - Insert one pull block per selected pin.
  - Confirm insertion Z and motion coordinates match the resolved Config.

## Assumptions

- The initial version targets FFF only. SLA support is out of scope.
- DynaPin Config is linked at machine preset granularity.
- The initial pin selection UI can be a `row,col` list input. Grid click selection is a later phase.
- DynaPin Config is not embedded into 3MF. The 3MF stores selected pins and enabled/disabled state; reproduction requires the same machine preset and Config.
