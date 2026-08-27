# DynaPin Origin Naming and Zero-Based Grid Design

## Goal

Make the DynaPin configuration self-explanatory by replacing the ambiguous `origin` / `physical_origin` pair with `pull_origin` / `support_origin`, and remove configurable row/column origin values so every grid starts at `(0, 0)`.

## Scope

This is an intentional configuration-schema break. Existing DynaPin JSON files using `origin`, `physical_origin`, `origin_row`, or `origin_col` will not be supported. The change covers the DynaPin runtime configuration, the bundled KP3S profile, tests, and DynaPin documentation.

The `Pin` type and user-facing selected-pin syntax remain row/column based. A pin is still identified as `row,col`; only the configurable grid origin is removed.

## Configuration schema

The `grid` section becomes:

```json
{
  "grid": {
    "pull_origin": {
      "y": 14.0,
      "z": 5.0
    },
    "support_origin": {
      "y": 18.0,
      "z": 4.0
    },
    "row_count": 10,
    "col_count": 14,
    "pitch": {
      "row_y": 12.4,
      "col_z": 7.4
    }
  }
}
```

`pull_origin` is the base coordinate for generated pull G-code. `pull_gcode.y_offset` and the other pull movement offsets retain their current meaning and are applied after the grid position is calculated.

`support_origin` is the base coordinate of the installed physical pin array. It is used to calculate physical pin positions and all support-related geometry, including support blockers, virtual pin-top support surfaces, model collision checks, and preview blocker boxes. The existing fixed `support_block_y_offset` remains unchanged and is applied after the support pin position is calculated.

The grid origin is always row `0`, column `0`:

```text
pull_y = pull_origin.y + row * pitch.row_y + pull_gcode.y_offset
pull_z = pull_origin.z + col * pitch.col_z

support_pin_y = support_origin.y + row * pitch.row_y
support_pin_z = support_origin.z + col * pitch.col_z
```

Candidate pins are enumerated as `row = 0 .. row_count - 1` and `col = 0 .. col_count - 1`.

## C++ model

`DynaPin::Config` will use these fields:

```cpp
int    row_count = 0;
int    col_count = 0;
double pull_origin_y = 0.;
double pull_origin_z = 0.;
double support_origin_y = 0.;
double support_origin_z = 0.;
```

`origin_row`, `origin_col`, `origin_y`, `origin_z`, and `physical_origin_y/z` will be removed. `candidate_pins()`, `pin_y()`, `pin_z()`, `pull_y()`, `pull_z()`, sorting, logging, and tests will use the new zero-based calculations.

## Error handling

The JSON loader will read only the new keys. No fallback aliases or migration behavior will be added. Existing validation for grid dimensions, pitch, support exclusion dimensions, bed range, and pull-front position remains in place. Missing numeric values continue to use the existing loader defaults unless the existing validation already rejects the resulting configuration.

## Documentation and fixtures

The bundled KP3S JSON and `resources/profiles/Kingroon/dynapin/README.md` will document the new schema and formulas. References to configurable row/column origins and old migration aliases will be removed or rewritten. Tests will assert that the grid starts at `(0, 0)`, that pull and support coordinates remain independent, and that the new field names produce the expected coordinates and G-code.

## Non-goals

- Do not rename `PullMoveConfig::y_offset`, `approach_y_offset`, or `z_offset` in this change.
- Do not change the physical `support_block_y_offset`.
- Do not change pin selection syntax, DynaPin preview comment syntax, support-selection behavior, or support geometry dimensions.
- Do not add compatibility parsing for the old schema.

