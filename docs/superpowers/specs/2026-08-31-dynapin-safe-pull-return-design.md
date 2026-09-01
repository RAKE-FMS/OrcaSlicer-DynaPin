# DynaPin Safe Pull and Return Design

## Goal

Prevent a DynaPin pull from crossing freshly printed geometry at the previous layer height, and resume normal layer travel from a position that matches the G-code writer's recorded state.

## Problem

`GCode::change_layer()` currently requests a lazy Z lift through `retract()`, updates the writer's logical Z to the next layer, and then appends DynaPin commands as raw G-code. Because the lazy lift is not emitted until the next writer-managed travel, the first DynaPin Y move runs at the previous physical layer height. After the pull, the printer is at the DynaPin disengage position while the writer still records the pre-pull position, so the next writer-managed travel also starts from stale coordinates.

## Selected Approach

`GCode::change_layer()` will wrap all DynaPin pulls due at the current layer in one safe movement envelope:

1. Preserve the existing layer-change retraction and wipe behavior.
2. Resolve all newly due DynaPin pulls before emitting any pull command.
3. Materialize the configured Z hop as an immediate vertical lift before the first DynaPin XY move.
4. Save the writer's resulting lifted XYZ position as the return position.
5. Emit every DynaPin pull due at this layer without returning between pins.
6. After the last pull, move XY back to the saved position while remaining at the DynaPin disengage Z.
7. Move Z back to the saved lifted Z.
8. Resume normal layer processing. Because the physical toolhead has returned to the same XYZ retained by the writer, no writer-state override is required.

The lift used for this safety envelope is a normal vertical lift, even when the configured automatic layer-change lift would otherwise use a spiral. A vertical move guarantees that no XY motion occurs before the nozzle has cleared the printed layer.

## G-code Shape

For one or more pins due at the same layer, the generated sequence will have this shape:

```gcode
; existing retraction and wipe
G1 Z<saved_lifted_z>             ; immediate vertical safety lift
; BEGIN_DYNAPIN_PULL ...
...
; END_DYNAPIN_PULL
; optional additional pull blocks at the same layer
G1 X<saved_x> Y<saved_y>         ; return at DynaPin disengage Z
G1 Z<saved_lifted_z>             ; restore the writer-known safe position
; normal next-layer travel and unretraction
```

The return XY move must be emitted before lowering Z. This keeps the nozzle above all printed geometry while crossing back from the front DynaPin position.

## Responsibilities

### `GCode::change_layer()`

- Detect and group pins that become due at the current layer.
- Force the pending configured Z hop to execute vertically before raw DynaPin G-code.
- Capture the lifted return position.
- Append the grouped pull blocks and one final return sequence.
- Mark due pins complete only when their pull block is included.

### DynaPin G-code generation

- Continue generating the physical approach, latch, pull, disengage, and DynaPin Z-retreat commands for an individual pin.
- Provide or support generation of the final return moves using machine coordinates and the existing DynaPin feed rates.
- Remain independent of extrusion retraction and layer-planning policy.

### G-code writer

- Remain the authority for configured Z-hop height and the saved return position.
- Not be force-synchronized to the DynaPin disengage position. Raw DynaPin motion must finish at the position the writer already records.

## Coordinate and State Rules

- The saved XY position is the toolhead position immediately after the layer-change wipe.
- The saved Z position is the next-layer logical Z plus the configured Z hop after the immediate lift has been materialized.
- Machine/plate XY offsets must be applied consistently with existing writer output when formatting the return coordinates.
- Multiple pins due at one layer share a single initial lift and a single final return.
- The final physical XYZ must equal the writer's recorded XYZ before normal layer travel resumes.
- The implementation must not reset or approximate `m_lifted`, `m_to_lift`, or other private lift bookkeeping through manual position synchronization.

## Edge Cases

- If no pin becomes due, layer-change output remains unchanged.
- If multiple pins become due together, all pulls occur inside the same safety envelope.
- If configured Z hop is zero, the implementation must not claim collision clearance. The generated behavior should remain deterministic and covered by a test; adding a new DynaPin-specific clearance setting is outside this change.
- Retraction and wipe must not be duplicated when the layer-change path already performed them.
- DynaPin preview markers and the per-pin pull event boundaries remain unchanged.

## Testing

Tests will verify observable G-code ordering and coordinates:

1. The first DynaPin Y approach is preceded by an emitted vertical Z lift.
2. The return XY move occurs after the final `END_DYNAPIN_PULL` and before the return Z move.
3. The returned XYZ equals the saved lifted position.
4. Two pins due at the same layer produce one safety lift and one final return, not one pair per pin.
5. A layer without due pins keeps the existing layer-change behavior.
6. Existing DynaPin pull marker and preview tests continue to pass.

The regression fixture should model the `RemoveGymnastBase` failure shape at the behavioral level: a layer-change wipe position followed by a DynaPin pull whose first Y approach would otherwise cross the just-printed layer.

## Out of Scope

- Changing pin selection, blocker geometry, support propagation, or KP3S physical coordinates.
- Adding a new user-facing DynaPin clearance option.
- Changing the internal DynaPin pull path between approach and disengage.
- Modifying unrelated layer-change, timelapse, or tool-change behavior.
