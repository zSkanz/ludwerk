# 0262 — What a wall hides is not drawn, and the culler pays for itself

- Status: accepted
- Date: 2026-10-26
- Builds on: ADR 0134 (one drawn position per instance), ADR 0182 (a run of
  instances drawn in pieces), ADR 0184 (`BasePart.Fade`)

## The question

A frame draws every part the camera's frustum holds. A street of houses seen
from inside one draws the houses behind its walls; a valley draws what is
behind the ridge. The renderer had no way to leave out what is hidden behind
something nearer: the frustum and the shadow cascades were all its culling.

Two families of occlusion culling are what an engine of this class has:

- **On the GPU, from a depth pyramid**: the last frame's depth, read back or
  tested where it is. Read back, it is a frame or two late, and what turns
  into view behind a corner appears late. Tested on the GPU in two phases,
  it needs the draws to be chosen by the GPU -- indirect draws for the whole
  world -- which this renderer, submitting its draws from the CPU, does not
  have.
- **On the CPU, from a small depth buffer of the biggest occluders drawn in
  software** each frame: no latency, the same on a phone as on a desk, and
  paid in CPU time rather than GPU time.

## Decision

1. **A software depth buffer, each frame.** Before the frame is submitted,
   the biggest occluders the camera sees are drawn into a depth buffer of 256
   by 128, and each draw the frustum kept is tested against
   it by its bounding sphere: the farthest an occluder could be over the
   pixels the sphere covers, against the sphere's nearest point. A draw
   behind is left out of the camera's passes; its shadow is still cast, as a
   caster outside the frustum's is. Written in this repository: no
   dependency.
2. **Conservative, never a false hide.** An occluder's faces that look at the
   camera are drawn flat, each at the farthest depth its plane has over a
   pixel. A pixel wholly inside a face takes that depth. A pixel on an edge
   two drawn faces share, and wholly inside the two, takes the farther of
   their depths -- so a box leaves no crack down its own corner, and a pixel
   half on each face is never nearer than either. A pixel the outline crosses
   -- an edge with no drawn face across it -- or a corner where more faces
   meet is left: part of it may be what is behind. Of all the occluders, the
   nearest is kept. A sphere that reaches the near plane, or that covers a
   pixel no occluder reached, is drawn.
3. **The occluders.** An opaque `Part` whose shape is a block -- exact, and
   the walls, floors and buildings of most games -- with a size on screen
   worth drawing. It need not be anchored: what is hidden is decided again
   every frame, from where each part is drawn that frame. A mesh is an
   occludee only, since the engine cannot tell a solid mesh from a hollow
   one. `BasePart.Occluder` (`Enum.OccluderMode`) says otherwise:
   - `Automatic` (the default): the rule above.
   - `Never`: this part hides nothing -- a block with a hole a player looks
     through.
   - `Always`: this part's box is solid, whatever its shape -- a building or a
     cliff made as a mesh -- and hides as a box does. The author vouches for
     it. An animated mesh, or one hanging as cloth, never hides: its shape
     leaves its box.
   The property is a look: it travels to every replica (protocol 67), is kept
   in a streamed chunk's record, and is in no hash.
   **The ground hides too.** A terrain's, as a grid of heights round the
   camera, 32 by 32 tiles of sixteen voxels -- half a kilometre at the
   default voxel. A tile's top is the highest point every one of its voxel
   columns is whole ground up to, without a break, from the first whole
   voxel; a corner is the lowest top of the tiles round it, so every triangle
   of the grid is inside the ground, and what it hides the ground drawn over
   it hides. Each tile is lowered by how far the level of detail drawn there
   may stray from the ground, its own or its parent's, which it slides
   toward, and is solid only where the ground is drawn. Eight voxels a tile
   was four times the triangles, most of them smaller than a pixel by the
   horizon, for ridges a little sharper. A block world's, as boxes: the
   opaque blocks within 64 of the camera merged greedily -- along x, then z,
   then up -- so a wall is one box; blocks are boxes, and a grid of heights
   lost a wall thinner than two of its tiles. Only blocks drawn as they are
   now count: a hole just filled is not yet drawn filled. Both are kept
   until the ground, what is drawn of it or the tile under the camera
   changes.
4. **It pays for itself or rests.** It measures what it saves against its
   own time: the draws it left out at what a draw cost to record last frame
   -- or, whatever that says, a tenth of what was in view hidden, which is
   GPU time the record time does not count: a street of houses drawn in a
   few instanced calls saves little recording and five sixths of its
   triangles. Losing ten frames in a row, it rests, and tries one frame a
   second to see whether it would pay again; that frame wakes it if it does.
   **Not woken by the view moving**: a camera following a hero across a game
   seen from above moves every frame, and woke an earlier version of this to
   find nothing, a quarter of a millisecond a frame. With fewer than 64 draws
   in view it does not test at all: hiding all of them would not pay for the
   occluders. `--occlusion-culling-always` tests every frame, for a picture
   gate.
5. **Seen.** The frame report and F3's Stats say what it hid, from how many,
   in how long, or that it rests and why. `GraphicsService.OcclusionCulling`
   turns it off for a game; `--no-occlusion-culling` for a run.
6. **On the job system.** The occluders' faces are gathered first, then drawn
   a band of sixteen rows a job, each band writing only its own rows; the
   spheres are tested 256 draws a job, each changing only its own draws. The
   nearest depth and the counts do not depend on which job ran first. A
   frame that tests costs about a third less than it did on one thread.
7. **Outside the world's hash** (R10): it decides what is drawn, never what
   happens. `BasePart.Occluder` is a `Presentation` property for the same
   reason, though it travels.

## Proof

- **Two picture gates**, each drawing the same 120 frames twice, with the
  culler and without it, and comparing every fourth picture pixel for pixel:
  `occlusion_gate` -- seven by seven blocks of houses, 400 small things in
  the yards and streets, a camera at a crossing turning a full circle every
  two seconds, so a corner sweeps past in a few frames -- and
  `occlusion_ground_gate` -- a terrain ridge and a wall of blocks, things
  behind each and towers over them, the camera turning and rising and
  falling over their lines. Every picture is the same, and each gate also
  asks that something was hidden.
- **The rasterizer's own tests**: a wall hides what is wholly behind it and
  nothing beside or in front; a thin post hides nothing it does not cover; a
  box seen across a corner keeps no pixel nearer than the box is over it,
  each corner of every pixel cast as a ray; a ridge of ground hides the
  valley behind it and not what stands over its line, and a hole in it lets
  the valley show.
- **Measured** (RTX 4070 Ti SUPER and UHD 770, 640 by 360, the frame
  report):
  - The urban scene: 111 of 175 draws hidden a frame (63%); the camera's
    triangles from 53,292 to 8,520; 0.46 ms of the culler's time. The same
    on the UHD 770, 98 of 160. The scene is too small to be held back by
    either GPU, so its frame time does not fall: the saving is for a GPU
    that is.
  - The ground scene: 21 of 111 hidden, 0.25 ms.
  - `examples/10-open-world`: 24 of 2,252 hidden when it tests, at 0.62 ms;
    it saves less than it costs and rests all but one frame a second.
  - `examples/14-voxels`, `13-terrain` and `06-scene`: nothing hidden, or too
    few draws to try; they rest.
  - The self-check's own test: a frame of 200 things in view and none
    hidden rests within ten frames and tries one frame a second after; nine
    things are never tested; a street hiding 150 of 170 never rests; and a
    rest ends the second after the street comes into view.
