# 0146 — Water bodies follow curves: rivers descend, lakes take any shape, and the editor draws them

- Status: accepted (to be built; see `docs/briefs/water-kickoff.md`)
- Date: 2026-10-01
- Decided by: the owner, on 2026-10-01, after a friend could not move a
  `Water` in the editor: *"tem que ser uma parada profissional, prática e
  fácil de editar no editor e por código"*. By his standing rule of
  2026-09-30, the choices below follow what professional engines do (the
  water bodies of the major engines: river, lake and ocean defined by
  splines, edited in the viewport, carving the ground), and were not put to
  him one by one.
- Amends: [0118](0118-water-is-one-wave-definition-read-by-the-renderer-and-by-physics.md)
  (its shapes, its river, and its "swimming is a follow-up").

## Context

What ADR 0118 built, and where it falls short of the bar:

| Today | The bar |
|---|---|
| A river is a **polyline**: straight stretches, a hard kink at every point (visible in `examples/31-lake-and-river`) | a smooth curve through its points, with a corner only where one is asked for |
| One `Size.X` width and one `Size.Y` depth for the whole river | width and depth per point |
| One `SurfaceLevel` for the whole river: **a river cannot run downhill** | the surface follows the points' heights; flow follows the slope |
| A lake is a **rectangle** (`Box`) | a lake is any closed shape, drawn by its outline |
| `Water` and `WaterPoint` cannot be clicked or moved in the editor (they are not placeable) | everything placed in the world is clickable and movable (the sweep ordered on 2026-10-01) |
| No editor tool: points are typed in Properties | a water tool, drawn in the viewport like the terrain brushes |
| The bed is carved by hand with the terrain tools | the water body can carve its own bed, with banks |
| No shoreline, nothing seen from under the surface, no swimming | foam where water meets ground, an underwater look, a character that swims |

## Decision

### 1. Shapes

`Enum.WaterShape` becomes:

| Item | What it is |
|---|---|
| `Ocean` | unbounded, round the camera, as today |
| `Lake` | **new**: the area inside a closed curve through its points, at `SurfaceLevel` |
| `River` | a ribbon along an open curve through its points (today's `Spline`, renamed) |
| `Pool` | a rectangle from `Position` and `Size` (today's `Box`, renamed) |

A scene that says `Spline` or `Box` reads as `River` or `Pool`. The old names stay
readable as aliases and are written under the new names.

### 2. The curve

- The points are the `WaterPoint` children, in child order, as today. A curve
  through them is a **centripetal Catmull-Rom** spline: smooth, passing through
  every point, no loops or overshoot. A point with `Sharp = true` makes a
  corner there.
- `WaterPoint` gains:
  - `Position: vector` -- **full 3D**. For a river, its `Y` is the surface's
    height there, so a river descends. For a lake, `Y` is ignored: a lake is level
    at `SurfaceLevel`.
  - `Width: number`, `Depth: number` -- a river's, at that point, interpolated
    along the curve. Zero (the default) means the `Water`'s `Size.X` and `Size.Y`.
  - `Sharp: boolean` (default false).
- `WaterPoint` becomes placeable: selectable in the viewport by its handle and
  moved by the gizmo (the sweep of 2026-10-01 applies to every class).

### 3. Rivers that descend

- The surface height is the curve's interpolated `Y`. The flow runs downstream at
  `FlowSpeed` scaled by the slope (a still pool where the river is level, faster
  where it drops), and its direction is the curve's tangent.
- A stretch steeper than a stated angle is a **cascade**: drawn as falling water,
  floating bodies carried down it.
- Where a river meets a lake or the ocean, ADR 0118's "the highest surface holds
  a point, and of two at one level the one that moves" still decides; the
  renderer blends the two surfaces across the river's mouth, with no seam.

### 4. Carving the bed

- `Water.CarveTerrain: boolean` (on by default for a `Lake`, `River` or `Pool`
  made in the editor; off for one made by code, which asks for it) and
  `Water.BankWidth: number`.
- When on, the water body carves the terrain under it to its depth, with a soft
  bank `BankWidth` wide, using the terrain's own edit path (ADR 0137's rules for
  edits, ADR 0144's streamed cells).
- **Re-carving** follows the body: moving a point or changing a width re-carves,
  as one undo step in the editor. Ground the carve removed is restored when the
  carve moves away; ground the person sculpted by hand afterwards is kept (the
  carve is a layer under hand edits, not a one-way dig).
- `Water:Carve()` re-carves on demand from a script.

### 5. How it looks

- **Rivers** draw their ripples moving along the flow (a flow map from the curve's
  tangent and speed).
- **Shorelines**: foam and a softer, lighter edge where the water is shallow over
  the ground, measured from the depth to the terrain or any part below.
- **From below**: a camera under the surface sees the water's fog and tint, and
  the surface from underneath; the transition at the waterline is clean.
- A lake's surface is triangulated from its curve, tessellated finely enough for
  its waves.

### 6. Swimming

`CharacterBody` gains a swim state: in water deeper than its chest, it floats,
`Move` steers it in 3D (the camera's look decides up and down), `Jump` rises to
the surface, and it leaves the water where the ground is shallow enough to
stand. `Swimming: boolean` (read), and the `State` enum gains `Swimming`. It is
in the simulation, the trace and rollback, and replicates like the rest of the
character.

### 7. In the editor

A **Water** tool, with the terrain brushes' feel:

- **River**: each click on the ground adds a point at the end, a click on the
  ribbon inserts one between two, Delete removes the selected one. Handles in the
  viewport drag a point along the ground (with height by Ctrl-drag or a field), and
  a width handle at each point drags sideways.
- **Lake**: click the outline point by point and close it on the first point.
- **Pool**: drag a rectangle on the ground.
- **Ocean**: one click places it.
- The ribbon, outline and surface are previewed live while they are edited.
- "Water at this height": click the ground to set `SurfaceLevel`.
- Every edit is one undo step. The whole body moves with the gizmo (its points
  move with it). Every string goes through i18n keys.

### 8. By code

Easy for the common cases, complete for the rest:

```luau
local river = Instance.new("Water")
river.Shape = Enum.WaterShape.River
river:SetPoints({ vector.create(0, 12, 0), vector.create(40, 9, 25), vector.create(90, 4, 30) })
river.Parent = workspace
river:Carve()
```

- `Water:SetPoints(points: {vector})` replaces the `WaterPoint` children;
  `Water:GetPoints(): {vector}`; `Water:AddPoint(position, index?)`.
- `Water:GetHeightAt`, `GetNormalAt` (as today), and new
  `Water:GetFlowAt(position): vector`, `Water:IsUnderwater(position): boolean`,
  `Water:GetPointAt(distance): (vector, vector)` (position and tangent along a
  river, for a boat that follows it).
- Every verb validates its inputs as the terrain's do (finite values, bounds).

### 9. Around it

- Points, widths, depths and the new properties replicate; the carve is the
  authority's and travels as the terrain's edits do (ADR 0135).
- Buoyancy, drag and flow stay in the physics step, deterministic, in the trace
  and the rollback snapshot (ADR 0118).
- Moving a point rebuilds only that body's mesh and its carve's chunks.

## Consequences

- A river can be drawn in seconds in the editor or in three lines of code, bends
  smoothly, runs downhill and carves its own bed; a lake can be any shape.
- `Spline` and `Box` scenes keep working under the new names.
- The determinism trace of the water scenario is re-recorded once if the curve
  changes a floating body's path, with the reason named.

## Not decided here

- Waterfalls as particle effects beyond the cascade surface; caustics on the
  ground under water. Later polish.
- An FFT ocean (ADR 0118's "not decided" stands).
