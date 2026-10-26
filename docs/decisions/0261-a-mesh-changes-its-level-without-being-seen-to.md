# 0261 — A mesh changes its level of detail without being seen to

- Status: accepted
- Date: 2026-10-26
- Builds on: ADR 0010 (the asset stack, whose compiler makes a chain of
  levels for every mesh), ADR 0174 (the depth prepass), ADR 0182 (a run of
  instances drawn in pieces)

## The question

Every mesh the engine imports is compiled with a chain of levels of detail,
and the renderer has drawn the coarsest whose error stays under a pixel. What
was missing was everything around that choice:

- It was made afresh every frame and every pass, from the distance alone. A
  mesh standing near the edge between two levels flickered between them as
  the camera breathed, and every change was a visible pop.
- `GraphicsService.ViewDistance` was saved and reported and did nothing: a
  pebble two kilometres off was drawn as faithfully as a house.
- A game had no way to say that a mesh's silhouette matters -- a sign, a
  landmark -- and must not be simplified.

## Decision

1. **Decided once a frame, kept from frame to frame.** Before the frame is
   drawn, each part's mesh is given its level (`LodTransitions`), written
   into its draw, and every pass -- the camera's, the shadows', the count of
   triangles -- reads that one.
2. **Hysteresis.** A mesh goes coarser only when the coarser level would
   still do seen a sixth nearer, and finer only when its level would not do
   seen a sixth further. Between the two it stays where it is.
3. **A crossfade, opaque.** Where the quality is above Low and the machine is
   not a phone, a change of level is a quarter of a second in which both
   levels are drawn, each through the other's holes of a four-by-four
   ordered dither: every pixel is drawn once, opaque, and its depth and light
   are a solid mesh's -- the two never blend, so nothing is sorted and no
   transparency appears. The level going out is drawn by the camera and casts
   no shadow. A draw fading is drawn alone, out of its instanced run, and
   writes its own depth as a cutout does. On Low and on a phone the change is
   a cut, made rarely by the hysteresis. A headless run cuts too: its
   pictures are of the frame they are taken on, not of how far a fade on the
   wall clock had got. A material with its own surface shader cuts as well.
4. **Distance by size** (`GraphicsService.ViewDistance`). A part or a mesh is
   drawn while it is at least two pixels across, divided by the view
   distance: a centimetre pebble goes at a few metres, a house kilometres
   away. A cut, not a fade: two pixels have nothing to pop, and a fading draw
   leaves its instanced run -- a first cut that faded put the far trees of
   the open world back into a call each, 188 calls up to 226.
5. **`MeshPart.LevelOfDetail`** (`Enum.LevelOfDetailMode`): `Automatic`, or
   `Finest` -- always the finest level, however far. A look: it travels to
   every replica (protocol 68), is kept in a streamed chunk's record, and is
   in no hash.
6. **Seen.** The frame report says how many changes of level the window had
   and how many things a frame were too small to draw.

## Proof

- A test of the band: a mesh whose coarse level does from fifty metres stays
  fine to 58 going out and coarse to 43 coming back.
- A test of the fade: the frame a level changes draws both, the new keeping
  none of the dither and the old all of it; a tenth of a second on, 0.4 and
  0.6; a quarter of a second on, one draw again.
- A test of the size: half a pixel is not drawn; at four times the view
  distance it is, as itself.
- A test of `Finest`: the finest level at five hundred metres.
