# DynaPin GUI Preview Plan

## Summary

Add a preview-only DynaPin visualization for physical pins. The physical pin models are prepared and placed by the user as normal 3D models. OrcaSlicer will let the user manually select those pin models in the 3D view, then show the selected pin being pulled during G-code preview when matching DynaPin comments are encountered.

This plan only covers GUI selection and preview visualization. It does not generate pin models, auto-place a pin array, or change the actual printed G-code behavior beyond reading DynaPin comments that already exist in the G-code.

## G-code Comment Contract

- A pull block starts with:
  - `; BEGIN_DYNAPIN_PULL ROW=<row> COL=<col>`
- The single G-code move that visually pulls the pin is marked with:
  - `; DYNAPIN_PULL_MOVE`
- The pull block ends with:
  - `; END_DYNAPIN_PULL`
- `ROW` and `COL` identify the pin position in the physical pin array, with the lower corner treated as `0,0`.
- Only the move marked with `DYNAPIN_PULL_MOVE` animates the pin. Other moves inside the block are treated as head preparation or cleanup moves and do not move the pin model.

## GUI Behavior

- Add a DynaPin selection mode in the 3D/preview UI.
- In selection mode, clicking an existing model whose name matches `dynapin_r<row>_c<col>` selects that physical pin.
- Selected DynaPins are highlighted.
- Models whose names do not match the DynaPin naming pattern are ignored by DynaPin selection mode.
- Add minimal controls:
  - Enable/disable DynaPin selection mode.
  - Clear the selected DynaPin.
  - Show/hide DynaPin preview overlays.

## Preview Behavior

- During preview loading, scan `GCodeProcessorResult::filename` line by line and extract DynaPin pull events from the comments.
- Use `GCodeProcessorResult::lines_ends` and each `MoveVertex::gcode_id` to map comment lines back to preview move IDs.
- For each valid event, record:
  - `row`, `col`
  - begin line / move range
  - pull move ID
  - end line / move range
  - pull move start and end positions
- When the preview slider is before the pull move, the selected pin remains at its original model position.
- While the slider is within the `DYNAPIN_PULL_MOVE`, interpolate the selected pin linearly from the pull move start position to its end position.
- After the pull move, keep the selected pin at the pulled position for the rest of the preview timeline.
- If the selected pin's `row,col` does not match the event's `ROW,COL`, that event does not affect the selected pin.

## Implementation Notes

- Keep the implementation local to the preview path, primarily around `GCodeViewer` and `GUI_Preview`.
- Add a small DynaPin preview state object instead of mixing event parsing and rendering directly into slider code.
- Suggested internal data:
  - `DynaPinEvent`: `row`, `col`, `begin_gcode_id`, `pull_gcode_id`, `end_gcode_id`, `start_pos`, `end_pos`
  - `DynaPinSelection`: selected `row`, `col`, and the selected GL/model reference
  - `DynaPinPreviewState`: resolves the visible pin transform for the current preview move
- Do not duplicate the selected model mesh in v1. Prefer either temporary preview transforms or a lightweight overlay that represents the selected pin's moved position.
- Invalid or incomplete DynaPin comments should be ignored with a warning log, not treated as fatal errors.

## Test Plan

- Parser tests:
  - Parse `BEGIN_DYNAPIN_PULL ROW=2 COL=5`.
  - Detect `DYNAPIN_PULL_MOVE` only inside a begin/end block.
  - Ignore incomplete blocks, missing row/col values, and blocks without a pull move.
- Preview state tests:
  - A matching selected `row,col` moves only during the marked pull move.
  - A non-matching selected `row,col` does not move.
  - Position before, during, and after the pull move is stable and deterministic.
- Manual GUI checks:
  - A model named `dynapin_r0_c0` can be clicked and selected.
  - A selected pin is highlighted.
  - Moving the preview slider shows the selected pin pulled from the side of the build volume on the marked move.
  - Non-selected pins remain stationary.
  - Hiding DynaPin overlays hides highlight and movement visualization.

## Assumptions

- The physical pin models and their array placement are prepared outside this feature.
- Each selectable pin model can be named using `dynapin_r<row>_c<col>`.
- The first implementation supports one selected DynaPin at a time.
- The pull visualization is a straight-line interpolation along the marked G-code move.
- Project persistence, pin array generation, row/column manual entry, and automatic pin assignment are out of scope for v1.
