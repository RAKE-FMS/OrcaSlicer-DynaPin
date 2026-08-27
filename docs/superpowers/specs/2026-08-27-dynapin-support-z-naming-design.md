# DynaPin Support Z Origin and Blocker Naming Design

## Goal

Make DynaPin support-blocker geometry express its upper Z boundary directly: `support_origin.z` becomes the blocker-top origin for row zero, `z_above` is removed, and blocker dimensions follow one consistent `<object>_<dimension>_<axis>` naming rule.

## Scope

This change covers the DynaPin runtime configuration, support geometry, automatic pin selection, pull scheduling, bundled KP3S configuration, tests, and DynaPin documentation. It does not change pull-G-code coordinates: `pull_origin` remains independent from support geometry.

## Current problem

The current implementation calculates a pin reference height with `pin_z = support_origin.z + row * pitch.row_z`, then derives the blocker range as:

```text
z_min = pin_z - pin_z_height
z_max = pin_z + z_above
```

`z_above` is an offset rather than an absolute upper boundary, and the legacy `blocker_z_max` field name suggests an absolute coordinate even though it stores that offset. `pin_z_height` and JSON `width_y` also do not follow a single naming convention.

## Proposed configuration

The bundled configuration uses the following schema:

```json
{
  "grid": {
    "support_origin": {
      "y": 18.0,
      "z": 7.55
    },
    "pitch": {
      "row_z": 7.4,
      "col_y": 12.4
    }
  },
  "support_exclusion": {
    "blocker_width_y": 12.4,
    "blocker_height_z": 5.0
  }
}
```

`support_origin.z` is the upper Z boundary of the support blocker for `row=0`. The upper boundary for a pin is:

```text
z_max = support_origin.z + row * pitch.row_z
```

`blocker_height_z` is the downward height of the blocker from that upper boundary:

```text
z_min = z_max - blocker_height_z
```

For the KP3S configuration, `support_origin.z = 7.55` gives the selected `4,4` pin an upper boundary of `7.55 + 4 * 7.4 = 37.15 mm`, and a lower boundary of `32.15 mm` with `blocker_height_z = 5.0`. The first coordinate is the Z row and the second is the Y column.

The naming rule is `<object>_<dimension>_<axis>`:

- `blocker_width_y`: blocker width along Y.
- `blocker_height_z`: blocker height along Z.
- `row_pitch_z` and `col_pitch_y` follow the same axis-suffix convention.

The old `z_above`, `z_max`, `z_range`, `width_y`, `y_width`, and `pin_z_height` configuration keys are not read as aliases. This is an intentional schema change; the bundled configuration and documentation are updated together.

## Runtime API and geometry

`DynaPin::pin_z()` and `Config::blocker_z_max` are removed. The support-side Z calculation is centralized in a `BlockerZRange` value and a `blocker_z_range(config, pin)` helper:

```cpp
struct BlockerZRange
{
    double z_min = 0.;
    double z_max = 0.;
};

BlockerZRange blocker_z_range(const Config& config, const Pin& pin);
```

The helper is the single source of truth for the per-pin support range. It is used by:

- `LocalBlocker` construction.
- `VirtualSupportSurface::print_z`, which becomes `z_max`.
- Preview `BlockerBox` bounds and support-side pin position.
- Physical-height sorting and automatic-selection diagnostics.
- G-code pull scheduling, which triggers when `z_max <= print_z`.

The pull path continues to use `pull_origin` and `pull_z`; it is not derived from the support blocker range.

`SupportMaterial.cpp` continues to consume `LocalBlocker::z_min/z_max` and `VirtualSupportSurface::print_z`. Its direct `DynaPin::pin_z()` debug output is replaced with the computed blocker range. The downward-propagation termination and blocker clipping behavior remain unchanged apart from the new boundaries.

## Behavior and safety

- A selected pin's support blocker covers exactly `[z_min, z_max]`; it must not be extended to `z=0`.
- Lower overhangs below `z_min` remain eligible for independent support generation.
- Support descending from an upper overhang terminates at the selected pin's `z_max` surface.
- Multiple blockers continue to merge only when their XY regions and Z ranges overlap.
- Pull G-code is emitted once per selected pin when the print reaches that pin's blocker top, while the actual pull move remains at `pull_origin` coordinates.

## Testing

Update focused DynaPin tests to verify:

1. `blocker_z_range()` calculates the expected top-origin range for multiple rows.
2. The KP3S configuration produces `z=32.15..37.15` for pin `4,4`.
3. Support blocker and virtual surface use the same `z_max`.
4. Pin sorting still follows physical row height without `pin_z()`.
5. G-code pull scheduling uses the range top and pull motion still uses `pull_origin`.
6. Existing stacked-pin and downward-propagation tests preserve their intended isolation and termination behavior with the updated fixture heights.

Run the focused DynaPin and support-material tests, then run the relevant CTest selection and a build of the affected targets.

## Non-goals

- The corrected DynaPin address semantics are `row → Z` and `col → Y`; the Y support offset, XY blocker dimensions beyond the naming change, and pull motion sequence are unchanged.
- No compatibility parser for the removed support-exclusion aliases.
- No change to Tree/Organic support behavior outside the existing DynaPin integration points.
