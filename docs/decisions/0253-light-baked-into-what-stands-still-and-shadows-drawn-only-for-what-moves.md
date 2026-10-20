# 0253 — Light baked into what stands still, and shadows drawn only for what moves

- Status: proposed
- Date: 2026-10-19
- Builds on: [0203](0203-light-that-bounces-irradiance-volumes.md) (the rays, and
  why lightmaps were left), [0207](0207-a-volume-kept-baked-to-a-file-and-its-mean.md)
  (a bake kept in a file, the editor's Bake and the command),
  [0250](0250-how-a-headsets-eyes-are-drawn-is-the-projects-to-set.md) (what a
  standalone headset's eyes cost), [0252](0252-a-standalone-headset-draws-every-other-frame-and-the-runtime-the-rest.md)

## Context

A standalone headset draws two eyes 72 times a second on a phone's GPU. On a
Quest 3 the sun's shadows cost the most of anything a frame does after the
surfaces themselves: a map of every caster, two cascades, sampled by every
lit pixel. ADR 0250 took the reach and the cascades down to what holds 72;
ADR 0252 halves how often a frame is drawn. What is left is to stop working
out, every frame, light that never changes.

ADR 0203 chose probes over lightmaps for this engine's worlds -- built by
scripts, out of parts placed and destroyed in play -- and named lightmaps as
what a game of large static places would want on top. A headset game is
that game more often than not: a room, a cockpit, a level built in the
editor, with hands, held things and characters moving through it. The
commercial titles on these headsets light their places this way.

## Decision

1. **What is baked is said, part by part.** `BasePart.BakedLight`, false by
   default: an anchored part with it set is static scenery -- its light is
   worked out ahead and kept, and it casts the shadows that are kept. A part
   that is not anchored, or not set, is lit and shadowed as now. Nothing
   changes for a game that sets none.
2. **A lightmap: the light that reaches each texel of a static surface**,
   the indirect light (bounces, as ADR 0203's rays work it out, the sky's
   and the lamps') and the static lamps' direct light, in an atlas of
   `Rgba16Float` pages. The sun's direct light is not in it: it is worked
   out in the frame, as now, so its highlight moves with the eye and
   `Lighting.ClockTime` still turns it -- what is kept of the sun is where
   it does not reach (item 3). `Lighting.BakeSun`, a sun baked whole for
   the weakest machines, is named here and left for later.
3. **A shadow mask: whether each texel sees the sun from where the sun was
   when it was baked**, past what stands still, one channel beside the
   lightmap; the file keeps that direction. A static surface reads its sun
   shadow from the mask, sharp where the texels are and soft where the rays
   say the sun is half hidden. The sun's map in the frame then holds only
   what moves -- hands, held things, characters -- over the small reach
   around the camera it needs (`[xr] shadow_distance` still says how far):
   a few draws instead of the level. A moving thing on a static surface
   shadows it from that map; the surface keeps the darker of the two.

   **The mask is only true near the direction it was baked for.** Within
   three degrees of it, it is used. Past that -- a game whose `ClockTime`
   moves the sun, or one that turns it -- the mask is set aside: what stands
   still goes back into the sun's map, and static surfaces are shadowed by
   the map as they are today, with the baked indirect light kept. The log
   says so once, with the angle. What that costs is today's: on a Quest 3,
   in the XR room, the shadow pass's 0.45 to 0.50 ms a frame and each lit
   pixel's taps of the map. A game with a moving sun bakes for the hour it
   spends most of its time at, or bakes no mask (`Lighting.BakeShadows =
   false`) and keeps only the indirect light.
4. **What moves is lit by what was baked around it**: the irradiance
   volumes (ADR 0203) give it the indirect light, as they do now, and each
   of their points keeps, beside its light, whether it sees the sun past
   what stands still -- so a character walking under a baked roof is in its
   shade. The bake places a volume over the baked parts' bounds where the
   scene has none, at the spacing the scene's size allows.
5. **Coordinates for the lightmap**: a second set of texture coordinates,
   in a vertex stream of its own that only a baked draw binds -- every
   other draw's vertices stay as they are.
   - A part's are the engine's: each shape's unit mesh carries a fixed
     layout (a box's six faces in one chart, a sphere's and a cylinder's
     wrapped), and a part is given one rectangle of the atlas, as large as
     its surface asks at `Lighting.LightmapDensity` texels a metre (16 by
     default, a texel six centimetres across; a part can ask for more or
     less with `BasePart.LightmapScale`, as a table a hand rests on does).
   - An imported mesh's second set is used where the file has one
     (`TEXCOORD_1`), checked for overlaps on import. Where it has none, the
     importer makes one: charts unwrapped and packed without overlap. That
     needs an unwrapper the engine does not have: xatlas (MIT), offline in
     `assetc` only and never in the runtime, a new dependency the maintainer
     approved on 2026-10-19 (R5), pinned and vendored as every other is
     (`third_party/manifest.json`, R13). A mesh the unwrapper cannot chart
     is lit as a moving thing is (item 4), and the bake says which.
6. **Where it is kept**: the bake writes the lightmap pages and masks to
   `asset://light/<scene>.lightmap` beside the scene, and each baked part's
   page and rectangle into a property of the part that is saved with the
   scene and not shown to scripts. `Lighting.Baked` names the file, as
   `IrradianceVolume.Baked` does (ADR 0207). A shipped game carries it in
   its pack; the export says when a scene's bake is older than the scene.

   **What it weighs.** Pages of 1024 by 1024 texels. A page holds 1,048,576
   texels, which at 16 a metre is about 4,000 square metres of surface: a
   house, or a few rooms and the yard around them. Kept as written, a page
   is 8 MB of half floats and 1 MB of mask; shipped, the export compresses
   it to UASTC HDR through Basis (ADR 0010's encoder, in `assetc`), and a
   machine turns that into ASTC HDR 4x4 where its GPU reads it (a Quest 3's
   is expected to; item 9 confirms it) or BC6H on a desktop: a byte a texel, 1 MB a page, and the mask
   in a single-channel format beside it. Where neither is read, the half
   floats are kept. A scene asks for four pages at most by default (4 MB on
   the device, compressed) and says so when its parts ask for more, which
   the density lowers.
7. **The bake**: the editor's Lighting has a Bake, with its progress; the
   command is `engine-host <project> --bake-lighting [--scene=...]`. The
   rays are ADR 0203's -- on the processor, through the job system, against
   each anchored mesh's own triangles -- sixty-four a texel, then a filter
   that keeps edges, and the charts' borders filled outward so a texel at a
   seam reads its own. A room of a million texels is seconds; a level,
   minutes. Nothing about the bake is in the simulation (R10): it is
   picture, and each machine reads its copy of the file.
8. **When the world changes what was baked**: a baked part that moves, is
   resized or is destroyed is drawn from then on as a moving thing is, and
   the light and shadow it left in the bake stay where they were until the
   next bake. That is the promise a lightmap is (ADR 0203), stated rather
   than hidden: `Lighting.BakeStale` is true and the log says which part.
9. **Where it is used**: wherever the scene has a bake, on every machine --
   one texture sample more for a baked surface, and a shadow map of only
   what moves. It is measured on a Quest 3 before it is called done: the
   XR room baked against unbaked, the eyes' GPU time and the shadow pass's,
   the sun turned past three degrees (the fallback's cost), the bake's time,
   its file's size shipped and the pages' memory on the device, and a look
   at the shadows at 0.35 m and the light at the corners.

## Consequences

- A baked level on a standalone headset spends its shadow budget on what
  moves, and gets bounce light with sharp contact shadows it cannot afford
  to work out every frame.
- The level is a promise: what it bakes must stay. A game that builds and
  breaks its world in play is lit as it is today.
- Two new kinds of file per scene and a step before export; a lightmap's
  memory -- a megabyte a page on a Quest, four by default -- is the price a
  phone pays.
- A moving sun keeps today's shadows: the mask is for a sun that holds.
- One dependency more, xatlas, in the import tool alone.

## Alternatives considered

- **Probes alone** (ADR 0203 as it is): no sharp bounced shadow, light
  through thin walls, and the sun's map still holds the whole level.
- **The sun's direct light baked too**: one sample cheaper, and the sun
  could never move nor its highlight follow the eye.
- **A shadow map of everything, cached between frames**: the cost moves to
  the frames the camera moves in -- every frame, in a headset.
