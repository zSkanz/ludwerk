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

- [ ] `Enum.WaterShape`: `Ocean`, `Lake`, `River`, `Pool`; `Spline`/`Box` read as
  aliases and written under the new names.
- [ ] `WaterPoint.Position` full 3D, `Width`, `Depth`, `Sharp`.
- [ ] Centripetal Catmull-Rom through the points; the renderer's ribbon and the
  physics' flow and height read the same curve (one function, tested to agree).
- [ ] Rivers descend; flow by slope and tangent; cascades.
- [ ] Lakes: a closed curve, triangulated, level at `SurfaceLevel`; inside test
  for buoyancy and `GetHeightAt`.
- [ ] The mouth: river into lake and ocean, blended with no seam.

## W2 — carving (§4)

- [ ] `CarveTerrain`, `BankWidth`, `Water:Carve()`; the carve as a layer under
  hand edits, restored when it moves away; re-carve on point moves; one undo
  step; streamed cells (ADR 0144) and edits (ADR 0137) respected.

## W3 — the look (§5)

- [ ] Flow-mapped ripples on rivers; shoreline foam by depth to the ground;
  underwater fog, tint and the surface from below; a lake tessellated for its
  waves.

## W4 — swimming (§6)

- [ ] `CharacterBody` swim state, `Swimming`, `State.Swimming`; in the trace and
  rollback; replicated; a test that walks in, swims across a lake and climbs
  out.

## W5 — the editor (§7)

- [ ] The Water tool: River, Lake, Pool, Ocean; handles; insert and delete;
  width handles; height by Ctrl-drag; "water at this height"; live preview;
  one undo step per edit; the gizmo moves a whole body with its points.
- [ ] An editor test driven by clicks and drags builds a river and a lake.

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

(Filled in as the work finds what this plan assumed wrongly.)
