# Water, made complete: the kickoff and the ledger

Decided on 2026-10-01 under the owner's standing rule (choose as professional
engines do, then tell him): *"tem que ser uma parada profissional, prática e
fácil de editar no editor e por código"*. The decision is
[ADR 0146](../decisions/0146-water-bodies-follow-curves-rivers-descend-and-lakes-take-any-shape.md).

**Place in the queue:** after the editor-i18n ledger (`docs/briefs/r3-editor-i18n.md`),
before F2. It replaces the smaller "water tool" item queued on 2026-10-01. The
placeability sweep (every class placed in the world is clickable and movable,
ordered the same day) is a defect and comes first, before E4.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A failing test or a failing picture first.
- The gallery approach of the terrain audit: a water gallery scene photographed
  at several distances and sun angles, with checks that fail before each fix.
- No determinism trace moves except the water scenario, once, with the reason.
- Every string through i18n keys; every class has an icon and a reference page.
- Full localgate, Linux included, before every push. Stage only your own files.

## W0 — the eyes

- [ ] A water gallery scene: a winding river descending a hillside with a
  sharp corner and a cascade, a lake of 12 points of irregular outline, a pool,
  the ocean, a river's mouth into the lake, carved banks, a floating crate and a
  boat. Photographed near, mid and far, sun high and low, and once from under
  the surface.
- [ ] Checks: the river's centreline curvature is continuous where no point is
  `Sharp` (no kinks); no seam at the mouth; the lake surface covers exactly its
  outline (a mask against the curve); nothing of the ground shows through
  the water where it is deeper than the surface.

## W1 — shapes and the curve (ADR 0146 §1–3)

- [x] **As built** (2026-10-01): `Lake` 3, `River` 4, `Pool` 5 are new items;
  `Box` and `Spline` stay as they were -- `Box` is `Pool`, and `Spline` is a
  river straight from point to point and level at `SurfaceLevel` -- so
  nothing made before changes shape. The curve is `scene::courseOf`, read by
  the simulation (`waterHere`), `GetHeightAt`, the picking and the picture.
  A river's height runs straight between two points' (it never climbs
  between points that descend); it runs at `FlowSpeed` where level and
  faster by four times its slope. Lakes: point-in-outline, and a surface that
  is a grid clipped to the outline. The editor draws all four.
- [ ] Later in W1: cascades; the mouth blended with no seam (W3 has the
  look); replication of `Width`, `Depth`, `Sharp` (W6).
- [x] `Enum.WaterShape`: `Ocean`, `Lake`, `River`, `Pool`; `Spline`/`Box` read as
  aliases and written under the new names.
- [x] `WaterPoint.Position` full 3D, `Width`, `Depth`, `Sharp`.
- [x] Centripetal Catmull-Rom through the points; the renderer's ribbon and the
  physics' flow and height read the same curve (one function, tested to agree).
- [x] Rivers descend; flow by slope and tangent. (Cascades: later, above.)
- [x] Lakes: a closed curve, triangulated, level at `SurfaceLevel`; inside test
  for buoyancy and `GetHeightAt`.
- [ ] The mouth: river into lake and ocean, blended with no seam.

## W2 — carving (§4)

- [x] **The bed is cut on demand** (as built, 2026-10-01): `Water:Carve()`,
  `Water.BankWidth`, and the Water panel's **Carve bed** -- one undo step,
  and none when there is nothing to cut. `scene::carveWaterBed` lowers each
  column the water covers to the bed (the surface at the edge, the depth a
  bank in) through `asset::writeHeights`, a tile of columns at a time; it
  only ever digs. Tests: `water_tests.cpp` (the profile, and that a second
  carve cuts nothing), `editor_tests.cpp` (the undo step).
- [ ] Later: `CarveTerrain` -- the carve that follows the body by itself and
  gives back the ground it took when the body moves away, as a layer under
  hand edits; cells of a streamed terrain that are not resident.

## W3 — the look (§5)

- [ ] Flow-mapped ripples on rivers; shoreline foam by depth to the ground;
  underwater fog, tint and the surface from below; a lake tessellated for its
  waves.

## W4 — swimming (§6)

- [ ] `CharacterBody` swim state, `Swimming`, `State.Swimming`; in the trace and
  rollback; replicated; a test that walks in, swims across a lake and climbs
  out.

## W5 — the editor (§7)

**Its first slice came first** (the queue change of 2026-10-01: what the
owner can see and use before what he cannot): the tool for the water there is
today -- a level river through its points, a rectangle, a sea.

- [x] **The Water tool, first slice** (`editor_water.cpp`, `Editor::driveWater`,
  the Water panel): River -- a click a point at the end, the next stretch
  shown before the click, a point dragged, a click on a point selects it for
  Delete, Enter or Escape or **New river** puts it down, a click on a river
  picks it up; Lake -- a rectangle dragged, level where the drag began, its
  corners handles; Ocean -- a click at the height, one sea a world. One undo
  step a click or a drag. The panel's numbers: surface height, width, depth,
  flow speed. The manipulator is hidden while the tool is in hand: its
  handles are the water's own.
- [x] An editor test driven by clicks and drags builds a river and a lake
  (`editor_tests.cpp`, eight cases), and both were drawn in the editor itself.
- [x] With W1: Pool and the lake's outline (clicks round a shore).
- [ ] Later: a click on the ribbon inserts a point; width handles; height by
  Ctrl-drag.
- Later, found while doing it: a point laid where the ground is higher than
  the river's level is under the ground (W1's descending rivers and W2's
  carving are the answer, not the tool); **Create Hills** leaves the editor's
  camera where it was, which is inside the new ground; selecting a `Water`
  opens the panel but does not pick the tool up.

## W6 — by code, docs and the close (§8–9)

- [ ] `SetPoints`, `GetPoints`, `AddPoint`, `GetFlowAt`, `IsUnderwater`,
  `GetPointAt`, `Carve`, with validated inputs; conformance specs.
- [ ] Replication of the new properties; the carve travels as terrain edits do.
- [ ] `docs/manual/world/water.md`: "In the editor" and "By code" sections;
  `examples/31-lake-and-river` rebuilt on curves, a descending river and an
  irregular lake.
- [ ] The package regenerated; the owner draws a river and a lake himself; the
  ledger closes on his word.

## Findings

- **A new project's camera stands at the world's zero, looking at the
  horizon** (W5, first slice): the plane a first river would be drawn on with
  no ground is never ahead of a ray from there, and the first click of the
  first river landed nowhere. With nothing solid under the pointer and zero
  not ahead, the tool draws on a level ten metres under the eye.
