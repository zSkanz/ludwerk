# The terrain and its editor, made right: the audit and the ledger

**By the owner's order of 2026-09-29**, after seeing his sculpted terrain at a
distance: *"nosso editor de terreno e terreno tem que ficar perfeito"*. The
audit below is ludwerk-08's, copied as it was written; this top part is the
ledger of the work it asked for, in the stages its prompt set (T0 to T6). Its
photographs are in `terrain-audit-2026-09-29/`, named in English; the audit
cites them as `photos/NN` by their number.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## The bar

A person sculpts, paints and flies over the terrain from 1 m to 2 km, at any
sun angle and any quality level, and never sees ground that disappears, thins
or jumps; a crack, a step or a notch where two levels meet; a shape that pops
when the camera moves; dots, dashes, blotches or triangles in the light that
are not in the material; a shadow that detaches from its caster, or darkens a
surface that faces the sun; a frame hitch from terrain work; or a tool that
does something the person did not ask for, or loses ground. A shape drawn far
away is the same shape drawn close, at lower resolution.

## What must hold at every stage

- A failing test or a failing picture first, for every item. A test that proves
  a defect not yet fixed goes in marked as an expected failure
  (`doctest::should_fail`; for the gallery, CTest passing only on its verdict
  of sky through the ground), so `main` stays green and the fix that makes it
  pass has to take the mark off.
- Visual changes are judged against the gallery references the owner approves
  (ADR 0038).
- A change of the voxel meaning or of the mesher's output that moves a
  determinism trace is re-recorded once, with the reason named.
- A new representation or level-selection rule gets an ADR amending 0082.
- No new dependency without the owner's approval. R7 holds. Port 7778, never
  7777.
- ludwerk-08 hears at the end of each stage, with the gallery's pictures.

## T0 — the eyes first

- [x] The gallery scene (`tests/screenshots/terraingallery`): 2 m and 4 m
  slabs, balls of radius 2, 4 and 8, two merged balls, a floating ball, an
  overhang, a cave, a 45-degree slope, a painted area of two materials, and a
  terrain with none.
- [x] Rendered headless at 5 to 2000 m, the sun at 15, 30 and 60 degrees from
  four sides, at quality low, medium and high: many pictures in one run
  (`--screenshot-every`).
- [x] The pixel check: the sky magenta and the ground white
  (`--debug-view=holes`), and no sky inside the ground (`imgholes`), nor where
  the same frame at full detail (`--terrain-detail=full`) has ground. In the
  local gate.
- [ ] Visual goldens, once the owner approves references.
- [x] Switches: `[graphics] contact_shadows`; the debug views of each node's
  level, the sky term, the shadow factor and occlusion.
- [~] CPU tests at every level, L0 to L5: every shape keeps triangles; flat
  ground within centimetres; the seam between L and L+1 closed, by a ray grid;
  a coarse cell's material is the surface's; the sky term matches the level's
  own geometry; winding agrees with the surface-net sign.
- [x] Streamed-project tests for every case of TA16 -- written in T1, as each
  fix's failing test (`field_streamer_tests.cpp`, `terrain_ground_tests.cpp`);
  (f), a cell past `MaxCellChunks`, is refused at save by code alone -- a test
  needs sixteen thousand chunks in one cell.
- [x] A flight benchmark over the gallery and the owner's place, in
  `docs/perf-baselines.md`: p95, p99 and the worst frame.

## T1 — P0s, security and data first

- [x] TA5, TA15: each axis bounded before it is multiplied; NaN and oversized
  values refused in `WriteVoxels`; `WriteHeights` and `GrowBall` on a voxel and
  chunk budget, refused with the keyed error the other edits use.
- [x] TA16, TA17c: every edit and whole-terrain action loads every cell it
  touches, or refuses before touching anything; the read verbs and the raycast
  load ground first; `VoxelSize` fixed while cells are on disk and a cell of
  other settings refused; `Clear()` drops the index; a moved terrain re-derives
  its index bounds; `MaxCellChunks` enforced at encode; the script and the
  editor say the same thing.
- [x] TA4: Smooth never changes a flat plane, never opens a hole, never digs
  below the plane it smooths towards, keeps volume within a stated tolerance,
  and converges; its blur capped where the code says, or stated.

### T1 as it stands

- **TA5 and TA15 (D363, D364)**: a region's count saturates instead of
  wrapping; `WriteVoxels` refuses a size or an occupancy that is not a finite
  number before it writes; `WriteHeights` estimates what it would lay and
  refuses past `MaxHeightVoxels` (four edits' worth -- four kilometres of
  rolling ground at a metre voxel); `GrowBall` is held to smoothing's bound.
- **TA4 (D365)**: Smooth is rebuilt. Each surface is smoothed on its own
  along the axis it faces, in flux form, never past the next surface in its
  row. Measured: a 4 m and a 2 m slab under `SmoothBall(c, 8, 1)` change not a
  voxel; twenty passes at 0.3 over a ball on flat ground never take anything
  below the ground and keep the ball's volume to 92 per cent; each pass moves
  less than the one before. The blur is a gaussian half the brush wide, **at
  most four voxels**, as the code, the API reference and the manual now all
  say. The gentle hill a held brush wears down loses half a metre in thirty
  stamps rather than 0.6: the volume the old blur threw away now goes round
  the brush's rim.

- **TA16 and TA17c (D366, D367)**: a load of ground is all or nothing, up
  to 4 096 cells -- a square four kilometres across -- or the edit is refused
  untouched, by `scene.err.terrain_ground_too_wide` from a script and the same
  words in the editor; the read verbs and the raycast load first, the raycast
  a kilometre at a time; `VoxelSize` is fixed while an index names cells, and
  a cell of other settings is refused; `Clear` drops the index; the streamer
  moves its terrain cells' bounds with the terrain; a cell past
  `MaxCellChunks` is not written. The editor's whole-terrain actions load all
  of the terrain first. `CellSize`'s doc says 256 cells, as the code does.

## T2 — the shape at a distance

- [x] TA1: coarse levels keep what is there -- a thin slab or ball thins to a
  cell, never vanishes while it covers a pixel (ADR amending 0082).
- [x] TA2: flat is flat at every level, under 2 cm at L0 to L5; seams closed
  from both sides.
- [x] TA6: the level chosen by projected screen error, with hysteresis and a
  geomorph; the vertical distance from the node's own box; the 64/128 m comment.
- [x] TA7: a coarse cell's material and paint are the surface's.
- [x] TA3, TA11: the sky term matches the geometry it lights; no fixed bearings;
  air under an overhang is air; a vanished feature leaves no imprint.
- [x] Mesh P2: winding from the edge sign, crease diagonals chosen
  consistently, normals that do not fall back to +Y, and a stated choice on
  feature-preserving placement.

### T2 as it stands

**ADR 0140**: a coarse level is the level-0 surface gathered. Its should-fail
marks in `terrain_levels_tests.cpp` are off, and the gallery's hole check
(`terrain_gallery_holes`) is an ordinary test.

- **TA1, TA2, TA7**: a coarse cell's vertex is the point nearest the level-0
  tangent planes inside it (QEM, the mean along what they leave free), its
  material the one most level-0 vertices carry, its paint theirs; a coarse
  edge is a quad each way the level-0 surface crosses it, so a slab thinner
  than a cell is a sheet with two faces. Flat ground is under 2 cm at L0 to
  L5; the tests hold slabs, balls, paint and a plate at every level.
- **Seams (TA2, ludwerk-08's ask)**: stitched, never skirted -- the skirts are
  gone from the mesher, the loader and foliage. A node gathers the cells it
  shares with a coarser node at that node's level, on its four sides **and
  its four corners**. No skirt can show: a test holds what a node draws to
  the collider's surface, index for index, and the gallery finds no sky
  through a seam.
- **TA6**: a node's error is its cells' largest (plane distance, half a cell
  where materials mix or paint lies); a node splits where that error covers
  more than 4, 3, 2 or 1.5 px (quality low to ultra), measured from its own
  box, with 1.25 hysteresis. **The geomorph (ludwerk-08's ask)**: each vertex
  slides onto its parent's cell between 85 and 100 per cent of the distance
  its node gives way at, and **a seam's vertex slides over the smallest range
  of the nodes of its level that draw it**, which each works out alike -- the
  tag rides in the vertex, the neighbours' ranges in the draw. The depth
  prepass slides with it, through a terrain pipeline of its own.
- **TA3, TA11**: the sky term is the drawn level's own columns (level 1's for
  level 0), every solid run of them, sixteen bearings.
- **Mesh P2**: winding from the edge's sign at every level; a quad split along
  the shorter diagonal only when it is clearly shorter (under 0.8 of the
  other), otherwise always the same one; a vertex with no gradient takes its
  faces' normal.
- **Proof in motion, without TAA** (the engine has none; FXAA is per frame):
  the gallery's flight (`tests/perf/terrainflight`), photographed every fifth
  frame with the sky magenta and the ground white -- 300 pictures, **not one
  pixel of sky enclosed by ground**. A seam's vertex is shown to be in one
  place in every node that draws it while it slides, whatever each node's
  range (`terrain_seam_tests.cpp`). A frame-to-frame count of changed pixels
  was tried as a pop detector, on the flight and on a slow dolly; slow motion
  moves silhouettes a pixel on some frames and not others, and the count
  flagged as much with the geomorph off as on, so it proves nothing and is
  not claimed.
- **The gallery at every quality**: no enclosed sky at low, medium, high or
  ultra; its slack for ground a coarse level drew thinner is the quality's
  budget over 0.85, rounded up -- what a level is chosen to be within -- and
  never under 3 px: at ultra's 1.5 a ball's top close up moved 2 to 3 px, a
  pixel past what an error measured over a cell says.
- **Performance** (`docs/perf-baselines.md`): the owner's place's flight went
  from a p95 of 7.4 ms to 22 ms and a worst of 25 ms to 60 ms. Stated as a
  regression; T5 moves meshing off the main thread.
- **ludwerk-08's check on the owner's place** (after the first T2 commit):
  - **Dark streaks down the flanks at 500 m and blocks at 1 000 m were the sky
    term**, three ways: a run no surface closed went up to the sky (a pillar
    every ray past it met); a coarse column's run round the point counted as a
    roof; and a sideways ray met the column it started in. Fixed, with a test
    each that failed first. The pictures are clean at 250, 500 and 1 000 m.
  - **The sawtooth far edge at 1 000 m is the streamed ground's edge**, not a
    level of detail: full detail draws the same notches, and with the terrain
    load radius at 3 km the edge is straight. The owner's place keeps cells only
    so far from the camera; what is drawn beyond them is nothing. T5's.
- **The gallery after T2**, beside the pictures from before any fix:
  `terrain-audit-2026-09-29/gallery-after-t2-5-15-30m.png`,
  `gallery-after-t2-60-120-250m.png` (no dark imprint under the floating ball)
  and `gallery-after-t2-500-1000-2000m.png` (the 2 m slab and the plain
  terrain still there at 1 000 and 2 000 m).

## T3 — light and shadow on terrain

- [x] TA8: a slope-scaled bias, correct culling, the geometric normal for the
  receiver's offset, no acne, shadows attached in every cascade (TA10).
- [x] TA9: contact shadows with a slope- and distance-aware bias.
- [x] Local lights push terrain in their atlas.
- [ ] TA12: no-material terrain plain matte grey.
- [ ] TA13 and render P2: the right channels, the triplanar sign, the grain
  frame, resampled layer arrays, guarded normals, no streaks at a low sun.
- [ ] CPU and GPU rules agree, by readback, paint included.
- [ ] Foliage shadows respect alpha and fade.

### T3 as it stands

- **The eyes first, again** (`tests/screenshots/terrainshadow`,
  `terrain_shadow_acne`): rounded hills never steeper than the lowest sun and
  balls floating over nothing, so nothing shadows a face turned to the sun --
  from 8 m to 900 m, the sun at 8, 12, 20 and 35 degrees, at two softnesses.
  `--debug-view=shadow` draws blue where the ground faces the sun, and
  `imgshadow` counts faces the map or the contact mask darkens; failing first,
  the map darkened up to 180 000 of a picture's 500 000 pixels at 8 degrees
  (355 000 at `ShadowSoftness` 1), the contact mask 5 600.
- **TA8, TA10 and the local lights** (ADR 0142, D368, D370, D371): back faces
  culled, each fragment pushed by its own depth slope times the cascade's
  filter reach and a texel, up to N.L 0.1; the local tiles by their own
  reach; the receiver's offset along the mesh's normal. Now 66 pixels at most,
  on the smallest ball's terminator. Shadows photographed attached at 22 and
  60 m under a 10 and a 20 degree sun, as before.
- **TA9** (D369): the contact ray ignores the plane it starts on, carried in
  one over depth to each pixel it reads. A bias grown by the depth buffer's
  slant was tried first and took the contact from balls standing on ground
  forty metres off -- the instanced golden showed it. Now 16 pixels at most.
- **The streaks of photos/08** (D372): not specular aliasing, and not the
  mesh -- the top of a filled block is flat to the vertex at every level, a
  test says so now. The grain's noise hashed its lattice in floats, and one
  point could hash to two values from two cells: the grain's normal, the
  noise's slope, drew a line along that edge. An integer hash; and
  `--debug-view=bend` with `imgsteps` in the same gate, 73 to 454 steps a
  picture before and none after.
- **Found on the way** (D373): since TA14, a headless `--screenshot` without
  `--screenshot-every` photographed no terrain. A run that takes a picture
  builds in the frame now.

## T4 — tools that do what the person asked

- [ ] TA17a: Add never puts ground where the person is not pointing at ground.
- [ ] TA17b: a click on a part never sculpts.
- [ ] Every item of the editor P2 list, one by one.
- [ ] Starter materials in one action.
- [ ] Refusals reported to scripts; `HeightAt` typed `number?`.
- [ ] The manual matches the tools exactly.
- [ ] The terrain editor's text through i18n keys (R3, the owner's ruling of
  2026-09-29).

## T5 — performance

- [ ] TA14: meshing and colliders off the main thread or on a budget; p99
  under 16.6 ms and no terrain spike in the worst frame, before and after.
  Meshing: done (ADR 0141), the p99 met, the worst frame not yet; colliders:
  not yet.
- [ ] The collider does not pay for the sky term.
- [ ] Continuous digging rebuilds only what changed (P4), with a baseline.
- [ ] TA18: an edit invalidates only the navmesh tiles it touches.
- [ ] The raycast caches the field's bounds per revision.
- [ ] Moving a terrain moves its colliders.
- [ ] Internal-edge removal on chunk meshes, with a sphere rolled across a seam.

### T5 as it stands

**TA14's meshing is pulled forward, ahead of T3** (ludwerk-08's check of T2 on
the owner's place: the building had to leave the frame before the owner flies
again). ADR 0141:

- **Every mesh is built off the main thread**, by a quarter of the workers (one
  to four), from a snapshot of the field whose chunks are shared until written;
  a chunk's digests are atomic and the gathered-surface cache is locked.
- **A node keeps two meshes and is drawn only with one built for the seams it
  is drawn with**; where there is none, the frame before's ground is drawn
  there and beside it, until every node drawn is. What is drawn is judged
  against the ground last put up, so it is of one revision. Tested in motion:
  every node drawn, every frame of a flight built off the thread, is built for
  the levels drawn beside it (`terrain_loader_tests.cpp`; it fails without the
  deferral).
- **`--pace=60`** for the flight, since a headless run flat out is a camera
  thirty times too fast. The owner's place, paced: a p95 of 5.0 to 6.0 ms and a
  p99 of 7.9 to 9.3 ms (22 and 33 built in the frame). **The p99 is met; the
  worst frame, 18 to 25 ms, is not yet free of terrain** -- scheduling the
  workers and putting meshes up are what is left.
- **Tried and left** (ADR 0141's context): drawing only once the whole
  selection is built drew nothing while the camera moved; building only the
  seams in the frame still cost 3 to 18 ms a frame; stitching on the GPU needs
  levels held one apart and a buffer per node.
- **Still on the main thread**: the collider (TA14's other half).

## T6 — the close

- [ ] The gallery and every CPU test green; the full local gate.
- [ ] The owner's place photographed again at the audit's distances, before and
  after side by side, from a copy.
- [ ] **The owner's own look**, packaged: he sculpts, paints and flies. The
  ledger closes only on his word -- the one gate item no machine can run.

### T0 as it stands

- **The gallery's verdict, today** (`terrain_gallery_holes`, nine distances at
  quality high): no sky seen THROUGH the ground at any distance, and ground
  lost from 250 m on -- 101 pixels at 250 m, 8 897 at 500 m (the whole 2 m
  slab and the no-material terrain gone), 4 310 at 1 000 m and 961 at 2 000 m,
  where one pixel of ground is left. It passes only on that verdict until T2.
- **The CPU tests' verdict** (`terrain_levels_tests.cpp`,
  `terrain_seam_tests.cpp`), each an expected failure until its fix:
  - thin shapes lose every triangle at coarse levels (TA1);
  - flat ground at 3.75 m is 8 cm off at L2, 19 cm at L3, 1.29 m at L4 and
    1.62 m at L5 (TA2);
  - paint vanishes at L4 and L5 (TA7);
  - a plate the coarse level does not draw still darkens the ground under it
    to a sky term of 0 (TA3);
  - rays across a seam see through it: none at L0/L1, 3 at L1/L2, 738 at
    L2/L3 (TA2).
  What already holds is asserted plainly: a thick block's coarse cells keep
  its material, and open flat ground sees the whole sky at every level. The
  ray grid is proved on a seam between two nodes of one level, which is closed.
- **The gallery before any fix**, as a player sees it at quality high, the
  sun at 30 degrees: `terrain-audit-2026-09-29/gallery-before-5-15-30m.png`,
  `gallery-before-60-120-250m.png` (the step in the slab's edge at 250 m) and
  `gallery-before-500-1000-2000m.png` (the 2 m slab and the plain terrain
  gone).
- **Still owed in T0**: the visual goldens (after the owner approves
  references), winding against the surface-net sign (with T2's mesh work),
  and the TA16 tests (T1). The flight benchmark is measured: the owner's place
  at a p99 of 10.6 to 11.4 ms and a worst frame of 20 to 25 ms, the nodes being
  meshed on the main thread (`docs/perf-baselines.md`).

## Findings

- **The gallery's first run found a mistake in the gallery, not the
  terrain**: a ball carved to make an overhang reached through the 4 m slab,
  and the hole it left was, correctly, sky. A scene that proves defects is
  looked at by eye first, at full detail, for holes of its own making.
- **A level budget by cell size cost ten times the nodes.** T2 first chose a
  level by the cell's projected size; on the owner's place it split every flat
  field to the finest level near the camera. By the error the level makes,
  flat ground stays coarse. A node not yet built counts its whole cell.
- **Per-node geomorph ranges open every seam while they slide.** Nodes chosen
  by their own error give way at different distances, and a seam's vertex is
  drawn by every node there. The gallery showed a crack along each seam; the
  shared rule closed them.
- **Stitching to the sides alone leaves a slit at a corner** whose diagonal
  neighbour is coarser than either side. A ray test from above does not see
  it -- the diagonal node's ring covers it -- and the gallery did.
- **QEM that drops a plane under 10 per cent of the largest recedes a rim.** A
  rim cell has many vertices on top and few on the side; the side's direction
  was dropped and the rim drawn half a cell in, which the error measure (plane
  distance at the vertex) did not see either. 2 per cent keeps it.
- **The first T2 flight was a p95 of 109 ms and a worst of 1.3 s.** The costs,
  in order of size: gathering chunks with no surface (air and buried rock,
  most of a view); one voxel read per level instead of per chunk; the surface
  gather on one thread; a 400 KB grid allocated and cleared per chunk; a
  point solved three times per vertex; the quads walked over the region's
  volume with a tree lookup per corner. Instrumentation that counted with
  shared atomics per vertex inflated what it measured -- measured again
  without it.
- **`meshField` wrote the field's cache**, and foliage meshes on workers: a
  race, found before it shipped. Meshing now only reads; what it reads is
  gathered first.
- **The streamer lets the ground go above about a kilometre**: the owner's
  place draws nothing from high in the flight, because its cells are evicted.
  Not T2's; noted for T5.

---

# The audit, as written: terrain and terrain editor, 2026-09-29

By ludwerk-08, at the owner's order: *"faça uma auditoria completa no sistema de
terreno não só os códigos testando também ... a correção tem que ser pelo outro
agente"* and *"nosso editor de terreno e terreno tem que ficar perfeito"*.

**Method.** Two parts:

- **Run.** The packaged engine (`Ludwerk-0.0.1-win64`, built 2026-09-29 17:43)
  was run on the owner's own saved place (`Documents\adadadw`) and on scenes
  made for the audit. Screenshots were taken headless at 14, 22, 45, 90, 120,
  160, 250, 500 and 1000 m, and each suspected cause was switched off in turn:
  `GlobalShadows`, the sky term (`Ambient = OutdoorAmbient`,
  `EnvironmentDiffuseScale = 0`), and quality `low`. Script probes measured
  collision, edits, bad inputs, timings and frame times during a camera flight.
- **Code.** Four read-only reviews, citing file:line: meshing and LOD;
  rendering and shadows; collision, edits, data and network; the editor.

Evidence marks: **[R]** reproduced by running · **[C]** found in the code ·
**[O]** seen in the owner's screenshots. The photographs sit next to this file
under `terrain-audit-2026-09-29/`.

## Why no earlier audit caught this

- **No image test of the terrain exists at all.** Nothing under
  `tests/screenshots`, `tests/look` or `tests/rendercapture` draws terrain.
  `screenshot_gate_contact` covers meshes only.
- **Coarse levels are barely tested.** Only one mesher test builds a coarse
  level (L1, flat ground plus one ball, 2 m tolerance). L2 to L5 are never
  meshed or checked.
- **Seams and hard shapes are not tested.** The seam tests and the
  closed-mesh/winding validation are level 0 only, with one union crease.
  Nothing tests a thin slab, a small ball, a floating ball, two merged balls, or
  flat-ground height across levels.
- **Streamed terrains are not tested.** No streamed-project test covers edits
  over more than 256 cells, `VoxelSize`/`Clear`/`Position` with cells on disk,
  or a `ReadVoxels`→`WriteVoxels` round trip on cells that are not loaded.
- **Earlier audits read code and ran unit tests.** None of them looked at
  sculpted terrain on a screen at several distances. That is where every one of
  the owner's complaints is.

---

## P0 — visibly broken, data or engine at risk

**TA1. Coarse levels delete anything thinner than about half a coarse cell.**
[R][C][O]

- **What happens.** The owner's ground (FillBlock-style, about 2 m thick)
  **vanishes entirely at 512 m and beyond**. At 1000 m only one loose plate
  remains (`photos/04`). Balls lose volume, go blocky, then vanish.
- **Cause.** Mip occupancy is a box mean (`engine/asset/src/terrain.cpp:381-383`)
  and the mesher thresholds at 0.5 (`terrain_mesher.cpp:669, 898-907`). A cell's
  mean is roughly `thickness / 2^L`.
- **Worked numbers for a 2 m slab:**
  - L1: survives, but thinner.
  - L2: survives or vanishes depending on alignment.
  - L3 and beyond: always vanishes.
- **The code's own comments claim the opposite:** "a thin wall thins rather than
  vanishing" (`terrain.h:279-283`, `terrain_mesher.h:46-49`,
  `terrain_loader.h:12-13`, ADR 0082 102-105).

**TA2. The seams between levels are open, and flat ground moves up and down with
the level.** [R][C][O] This is the owner's *"recorte"*, even on spheres
(`photos/03`, `photos/07`).

- **Seams are not closed.** Only the finer node draws a skirt, and it hangs
  downward (`terrain_loader.cpp:544-565`, `terrain_mesher.cpp:922-1002`).
  Wherever the coarse side is higher, the gap is open.
- **Surfaces miss each other.** On slopes and curves, the ring vertices of the
  two levels miss each other by up to half a coarse cell.
- **Flat ground height error.** From L2 up, the linear crossing (`crossingAt`,
  `terrain_mesher.cpp:53-59`) is biased by `s·u(0.5−u)/(0.5+u)`: about ±0.34 m
  at L2, ±0.69 m at L3, ±1.37 m at L4 and ±2.7 m at L5. The result is steps at
  every seam, and a whole node that pops in height when its level changes.

**TA3. The baked sky term is computed against level-0 geometry but applied to
coarse vertices, so distant shapes get pockmarks.** [R][C][O]

- **The proof, by running.** At 1000 m the owner's balls are covered in dark
  dots with shadows on (`photos/04`) and identically with shadows off
  (`photos/05`). With the sky term neutralised, the dots are gone (`photos/06`).
- **Cause.** The openness rays read level-0 `columnTop`/`columnBottom`
  (`terrain_mesher.cpp:416-427`), point-sampled one column per 2^L, with a lift
  (1.5 m) and ray stops fixed in metres (`:293, :555`). A coarse vertex sitting
  below the level-0 top reads as "under a roof".
- **More effects.** A feature that vanished at a coarse level still blocks rays,
  leaving a dark imprint with nothing above it: a candidate for the owner's dark
  oval. Nearest-column truncation (`:606-607`) gives dashes along creases.

**TA4. Smooth digs holes and erodes ground.** [R]

- **A hole in flat ground.** `SmoothBall(center, 8, 1)` on flat ground 4 m thick
  **made a hole through the whole slab**: the raycast at the centre found
  nothing, and the ring beside it dropped 0.35 m.
- **Repeated smoothing digs below the ground.** Five passes at strength 0.3 over
  a radius-4 ball took the centre from 7.94 m to −0.76 m, below the flat ground
  at 0. The ring at 5 m sank a little more on every pass (−0.03, −0.07, −0.10,
  −0.14, −0.17).
- **This contradicts the manual:** *"Softens bumps, fills pits and rounds off
  edges. Flat ground stays exactly where it is."*
- **No other finding covers it.** The code review did not catch it, and no test
  covers the volume or the flat plane under Smooth.

**TA5. `ReadVoxels`/`WriteVoxels`: the region bound can be bypassed by integer
overflow.** [C]

- **Cause.** `regionOf` clamps each side to about 2^26 voxels, and
  `VoxelRegion::count()` multiplies three of them as `usize`, which wraps
  (`engine/script/src/instance_binding.cpp:2125-2155, 2170, 2227`).
- **Effect.** A region that wraps passes the check, and then the loops run the
  true sizes: an uninterruptible C++ loop, with `int at` overflowing.
- **A second defect.** `WriteVoxels` casts a raw float to `i32`
  (`static_cast<i32>(max(floor(size.x), 0))`). A huge value or NaN is undefined
  behaviour.
- **The fix.** Bound each axis first, or compute the product in double.

## P1 — clearly wrong

**TA6. LOD selection pops: a fixed distance, no screen error, no hysteresis, no
geomorph.**
- **The rule today.** A node splits at `distance < 2·width`
  (`terrain_loader.cpp:405`). That gives L0 under 128 m, L1 at 128–256 m, and so
  on, each level about 7–14 px per cell at 1080p. The vertical distance uses the
  whole field's extent, not the node's.
- **What is missing.** No hysteresis band, no vertex morph, no crossfade. Four
  children swap at once, and the seams move with the camera.
- **A stale comment.** "full detail out to about 64 m" in
  `terrain_loader.h:69-72` is wrong; the code gives 128 m.

**TA7. A coarse cell takes its material and paint from its lowest full voxel,
not from the surface.** `prepareMip` picks the first maximum in walk order
(`terrain.cpp:362-377`). The ground's material climbs into the lower part of
balls in steps, and paint disappears at L2 and beyond. This explains the
brownish stair-steps on distant balls when the materials differ.

**TA8. Sun shadows on terrain: no culling, and a fixed push with no slope
term.** [C]
- **Cause.** The terrain shadow pipeline has `CullMode::None`
  (`renderer_default.cpp:4157-4166`). The only caster defence is
  `push = 6·texel/depthRange` (`:4598-4602`, `terrain_depth.hlsl:39`), and the
  RHI has no depth-bias state (`rhi/descs.h:144-155`).
- **Effect.** Acne starts at N·L below about 0.3 (at about 0.75 with
  `ShadowSoftness = 1`). It follows the flat facets, so the terminator looks
  faceted.
- **A related defect.** The terrain passes the bent `shadingNormal` into the
  shadow normal offset (`terrain.hlsl:418-421` → `engine_brdf.hlsli:393`). The
  offset should use the geometric normal.

**TA9. Contact shadows use a constant bias, which gives a stipple near the
terminator at medium distance.** [C]
- **Cause.** The bias is `0.01 + 0.0015·d` with no slope term, the step is
  jittered per pixel by IGN, and the result is combined as
  `min(shadowMap, contact)` (`contact_shadow.hlsl:77-99`,
  `engine_forward.hlsli:292-295`).
- **Why it matches the owner's dots.** The error grows with distance and fades
  out at 45–60 m. That is the owner's dots at medium distance, gone up close
  (the screenshots of the balls in the editor).
- **Not isolated by running.** There is no `graphics.contact_shadows` switch to
  turn it off alone. This is the primary suspect for the medium-distance dots;
  TA8 is the co-suspect.

**TA10. The far cascade's push reaches about 2.1 m, so shadows detach from their
casters.** [C] This is a candidate for the dark oval with nothing above it. The
cascade blend mixes two pushes, so a shadow changes shape across the band.

**TA11. The sky term uses 8 fixed bearings over a solid column map.** [C]
- **The model.** Air under an overhang counts as rock
  (`terrain_mesher.cpp:522-637`).
- **Lobes.** Ground near a tall shape darkens in lobes along ±X, ±Z and the
  diagonals, out to 12 m.
- **Visible triangles.** The value is per vertex, so ambient light shows the
  triangles and creases.

**TA12. A terrain with no materials is not "plain matte grey".** [R][C][O]
`terrainVariation` tints the default grey warm and cool, by about ±6% in
brightness and about 7% in R/B (`terrain.hlsl:379, 413`,
`engine_terrain_surface.hlsli:196-227`). It reads as brown blotches, and on
balls the triplanar blend makes them ball-sized. The ADR 0113 amendment says
plain matte grey.

**TA13. Terrain reads the R channel of a metallic-roughness map as occlusion and
as height** (`terrain.hlsl:259, 357-371, 422`). A user material with an
ordinary glTF map (R = 0) gets its ambient × 0.4, and its height blend always
loses. The engine's own sheets are packed differently and are fine.

**TA14. Meshing hitches on the main thread.** [R] Flying a camera from 1200 m
down to 60 m and back over the owner's place (headless, 1500 frames):
- median 1.44 ms;
- **p95 9.8 ms, p99 17.6 ms, worst 29.9 ms**;
- drawing on the CPU at p95 8.9 ms.

The spikes come from LOD rebuilds. Also: the collider rebuild pays for the
renderer's sky term (`physics_sync.cpp:1083` → `meshField` always builds the
column map and `skyVisibility`), and continuous digging (the P4 still open from
the earlier terrain audit) rebuilds whole read spans for meshes and foliage.

**TA15. Some edits have no memory bound.** [C]
- **`WriteHeights`.** It never refuses (`terrain_edit.cpp:482-622`). A
  two-entry table at voxel 0.1 with a ±1e6 band writes about 2e7 voxels into
  about 625k chunks; a 4096² checkerboard at default settings is about 262k
  dense chunks. It also feeds RaiseBall's solid-column window.
- **`GrowBall`** (the editor's Raise and Lower). It copies `(box+2)^3`
  (`:1102-1107`): about 0.5 GB and 1.3e8 chunk lookups in one call.

**TA16. Streamed terrains lose ground.** [C]
- **(a) A large edit is capped at 256 loaded cells.** `loadGround` stops there
  (`world.h:989`, `field_streamer.cpp:303-326`). An edit covering more cells
  writes chunks holding only the edit over the unloaded ones, and that loss is
  saved.
- **(b) `VoxelSize` can change while the ground is still on disk.**
  `setTerrainVoxelSize` checks `field.empty()`, and `materialize` never compares
  a cell's settings with the terrain's (`native_accessors.cpp:537`,
  `field_streamer.cpp:328-352`). The ground comes in at the wrong scale and is
  saved that way.
- **(c) `Terrain:Clear()` leaves `cellIndex`**, so cells that were never loaded
  stream back in.
- **(d) Moving a streamed terrain leaves the index bounds at the old origin**
  (`native_accessors.cpp:565`, `streaming.cpp:214`). Ground near the player
  stops streaming in.
- **(e) The read verbs do not load ground first.** `ReadVoxels`, `HeightAt`,
  `ApplyRules` and the raycast act on loaded cells only. The documented
  `ReadVoxels`→`WriteVoxels` copy idiom over an unloaded region therefore writes
  air over ground.
- **(f) A cell can save with more than `MaxCellChunks`**, and then it never
  loads. The limit is checked on decode only (`terrain_cell.h:69`,
  `field_streamer.cpp:146`). The save that follows refuses to overwrite the
  unreadable file, so the ground is lost.

**TA17. The editor's tools do what the person did not ask.** [C][O]
- **(a) Add puts ground in the air.** Once a stroke is running, a ray that
  misses the ground aims at a plane at the stroke's starting height, up to
  512 m away (`editor.cpp:4338-4365, 4549-4557, 4589-4601`). Held still there,
  it stamps a ball in mid-air that then grows. While dragging, it walks a bridge
  of balls into the sky. This is the owner's floating ball. The plane lock is on
  by default and hidden under Brush > More. Its tooltip and `editor.h:2267`
  describe a different height from the one the code uses.
- **(b) Clicking a part sculpts the ground behind it.** `driveSculpt` runs
  before the pick and raycasts only the terrain (`engine.cpp:3450-3458`). The
  manual says clicking anything else puts the brush down.
- **(c) Three "whole terrain" actions only touch loaded cells** (TA16e):
  Replace Material everywhere, Apply Rules to voxels, and Export Heightmap. The
  export has black holes where cells are not loaded.

**TA18. Every terrain edit, and every cell streaming in, invalidates every
navmesh tile.** The terrain static has infinite bounds and its hash is
`fieldRevision` (`recast_navigation.cpp:583-596, 626-650`).

## P2 — polish, and the rest

**Mesh**
- The quad diagonal alternates along creases, giving a sawtooth ridge (the
  dashed line in `photos/02`, a candidate).
- Winding comes from a normal heuristic instead of the exact sign of the
  surface-net edge (`terrain_mesher.cpp:828-843, 878-891`).
- Normals fall back to +Y on thin features.
- Silhouettes are cell-resolution polygons, with no feature placement
  (faceting).
- While nodes load, levels more than one apart can touch with a skirt too short
  (`terrain_loader.cpp:369-378, 415-423`).

**Render**
- The grain-nudge tangent frame is not orthogonal, and it degenerates on ±X
  walls.
- The triplanar normal bend has no V-sign handling, so glTF normal maps are lit
  from the wrong side along one axis.
- Layer arrays come from one bilinear blit from mip 0 into 512² (moiré on
  sources over 1024; non-square sources are stretched).
- The fallback arrays are created without contents.
- `normalize(input.Normal)` has no guard; a NaN can spread through bloom.
- CPU and GPU rule evaluation differ in four ways: slope source, blend, paint,
  and no agreement test (ADR 0113 §2 promises one).
- Foliage shadows ignore alpha and fade, so cards cast solid rectangles.
- There is no crossfade between LOD nodes.
- f32 positions and noise quantise beyond about ±16 km.
- Local-light shadows on terrain have no push.
- **Bright streaks on flat ground at a low sun**, at 22 m, in both quality
  levels (`photos/08`) [R]. Likely specular aliasing of the grain nudge.

**Data and API**
- `RaycastResult.Material` is computed for terrain and thrown away (ADR 0117 not
  yet built). The eyedropper samples a fixed 0.25 m inside, whatever the voxel
  size, and picks the stored material, not the drawn one.
- Raycast and collider are level 0, while the drawn surface beyond about 128 m
  is L1 or coarser. A character there floats or sinks by a fraction of the
  coarse cell.
- Each terrain raycast recomputes the field's bounds over every chunk.
- Moving a terrain re-meshes its colliders instead of moving them. No
  `mEnhancedInternalEdgeRemoval` is set, so bodies may snag at chunk seams.
- The world hash includes streamed ground, whose arrival depends on time
  (speculative, R10).
- **Bad inputs fail silently [R].** `FillBall` with a NaN centre, a NaN radius,
  an infinite radius, or above `MaxHeight` returns 0 and says nothing. The
  script cannot tell a refused edit from an edit that changed nothing.
- `HeightAt` is typed `: number` but returns nil.
- Stale docs:
  - `CellSize` "sixteen cells or more" (the threshold is 256);
  - the `SmoothBall` doc ("average of its neighbours");
  - `WriteVoxels` ignores `MinHeight`/`MaxHeight`, which the doc says are never
    written past.
- Editor Clear and Save As leave orphan cell folders on disk.

**Editor**
- The default sculpt tool is Add, not Raise.
- Hills and heightmap import on a new world take two undo steps, and they
  ignore a refusal from `writeHeights`.
- `applyTerrainRules` backs out a no-op with `undo()` instead of `retract()`,
  wiping the redo stack. It also reports a refusal as "already what the rules
  draw".
- Clear All Ground, Replace with Flat Ground and Replace with Hills act with no
  confirmation. Clear All Ground sits under Export Heightmap.
- Export writes MinHeight..MaxHeight but import defaults to 0..64 m, so the
  round trip fails by default. Export silently overwrites.
- The manual is stale on materials: the panel no longer makes them; Content's
  right-click does. Giving a new terrain the eight starters takes eight rounds
  of clicks.
- Setup's labels and limits disagree:
  - voxel size stops at 8 m in the UI but the manual says up to 64;
  - "Clear Terrain" vs "Clear All Ground";
  - "Settings" vs "Setup".
- Rule min/max are not kept in order, so a rule silently draws nothing. "Add
  Rule" assumes layer 4 is snow.
- Flatten's fixed height starts at 0 m, not at the ground.
- Smooth's blur is capped at 4 voxels (the comment says six), so a big Smooth
  barely softens.
- The foliage brush chip shows with the panel closed. "Grow foliage" only
  restores thinned density; it does not plant.
- The ring stays behind when the pointer leaves the viewport. Shift has three
  meanings. The chip shows with no terrain in the world.
- The brush chip is cut off at the right edge in a narrow viewport. The owner's
  screenshot shows it cut at the top-left: the cause is not settled; check it
  docked and in F3, at 100% and 150% scale.

## A decision for the owner (not a fix)

- **Does R3 (no hard-coded user-facing strings) apply to the editor?** ADR 0046
  says it does not. Two later briefs (`atmosphere-post-kickoff.md:35`,
  `user-shaders-kickoff.md:41`) say every editor message is an i18n key.
- **What it means today.** The terrain editor alone has about 280 English
  literals plus 33 status messages. The i18n lint only looks at `log`,
  `makeError` and `raise`, so it cannot see them.
- **If the answer is yes,** the lint has to learn ImGui calls and
  `EditorStatus`, and about 1,000 literals across the editor move to keys.

## What checked out

- A character dropped on flat ground, on a ball's top and side, on a floating
  ball and on a 50 m ball **stands exactly on the surface a raycast finds**
  (gap under 1 cm) [R].
- Oversized edits are refused with a keyed error. The material id and the
  editor's brush radius are clamped. Cell writes go through a temporary file and
  a rename.
- The editor keys, Esc, one undo per stroke, and the ring matching the edit all
  match the manual.
- The owner's 256 cell files are expected: a 1024 m square at voxel 1 is 16 × 16
  cells.
