# DynaPin row/column axis correction

## Goal

Align DynaPin's logical grid names with the physical array: `row` is the vertical Z direction and `col` is the horizontal Y direction.

## Current issue

The implementation currently maps `row` to Y and `col` to Z. This makes a physically vertical grid use the conventional row/column names in reverse. The existing mapping is repeated in pull G-code, support blocker geometry, automatic candidate enumeration, tests, and the bundled KP3S configuration.

## Design

The corrected mapping is:

```text
row -> Z
col -> Y

pull_y    = pull_origin.y    + col * pitch.col_y + pull_gcode.y_offset
pull_z    = pull_origin.z    + row * pitch.row_z
support_y = support_origin.y + col * pitch.col_y
support_z = support_origin.z + row * pitch.row_z
```

The configuration schema follows the corrected semantics:

- `row_count` is the number of Z levels.
- `col_count` is the number of Y positions.
- `pitch.row_z` replaces `pitch.row_y`.
- `pitch.col_y` replaces `pitch.col_z`.

The KP3S grid becomes 14 rows (Z) by 10 columns (Y), retaining the existing physical pitches of 7.4 mm in Z and 12.4 mm in Y.

## Scope

Update `DynaPin::Config`, coordinate helpers, automatic candidate enumeration, sorting diagnostics, support blockers and virtual surfaces, pull G-code, the KP3S JSON and README, focused DynaPin tests, support-material tests, and all DynaPin design/implementation documentation that states the old mapping. Preview event syntax remains `ROW=<row> COL=<col>`; only the coordinate meaning changes.

This is an intentional schema change. The old `row_y` and `col_z` keys are not accepted as aliases, because retaining them would preserve the misleading naming.

## Verification

Focused tests will assert that incrementing `row` changes only Z and incrementing `col` changes only Y, that the corrected KP3S dimensions enumerate `(0,0)` through `(13,9)`, and that support and pull coordinates use the same corrected address. Existing DynaPin support and preview tests will be updated and run, followed by the affected target build.
