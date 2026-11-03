# Drawing a large world

A world bigger than a screen asks four things of the engine: not to draw what
is hidden, to draw what is far with less, to let go of what it has left
behind, and to keep its textures within the memory it has. Each is automatic,
each says what it did in the frame report (`--frame-report=1`, and the F3
overlay's *Stats*), and each has a setting for a game that knows better.
`examples/49-far-city` shows all four on one avenue.

## What a wall hides is not drawn

Every frame the engine draws the biggest things in view into a small picture
of depth, in software, and tests everything else against it. What is wholly
behind a wall, a hill or a block of a block world is left out of the camera's
passes; its shadow is still cast.

It never hides what is seen: a pixel counts as covered only where the whole
pixel is inside a surface. Two tests draw the same frames with it and without
it and compare them pixel for pixel.

What hides what:

- A `Part` whose shape is a block and which is opaque. It need not be
  anchored.
- The ground of a `Terrain` near the camera, and the blocks of a block world.
- Any part you mark with `api:BasePart.Occluder = Always`. A `MeshPart`
  then hides as the box its mesh fills, so use it on a mesh that is solid,
  such as a building or a rock. Anything inside the box is treated as hidden.
  `Never` stops a block from hiding anything: a block with a hole the player
  looks through.

It pays for itself or it rests. It weighs what it saved against its own time,
and in a game where little is behind anything (one seen from above, say) it
stops testing and tries one frame a second to see whether it would pay again.
With fewer than 64 things in view it does not test at all. The frame report
says either "Occlusion culling: N of M draws hidden a frame" or that it rests.
`api:GraphicsService.OcclusionCulling` turns it off.

## A mesh changes its level of detail without a pop

Every mesh the engine imports is compiled with a chain of levels, each with
fewer triangles, and the level drawn is the coarsest whose error is under a
pixel. A mesh keeps its level within a band, so one standing at the edge
between two levels does not flicker between them. Where the quality is above
Low and the machine is not a phone, a change of level fades over a quarter of
a second: both levels are drawn, each through the other's holes of a fine
pattern, so nothing pops and nothing turns transparent. On Low and on a phone
the change is a cut.

- `api:MeshPart.LevelOfDetail = Finest` keeps a mesh at its finest level at
  any distance, for a landmark or a sign whose shape a player reads.
- `api:GraphicsService.ViewDistance` decides how small a thing may be on the
  screen and still be drawn: two pixels across, divided by the view distance.
  At 2 things are drawn twice as far.
- `api:GraphicsService.LODBias` moves where every level changes.

The frame report says how many changes of level there were and how many
things a frame were too small to draw.

## What the world no longer needs is let go

A streamed world loads each cell's meshes as the cell comes in. When the last
cell that wears a mesh is streamed out, and nothing else in the world has worn
it for ten seconds, the mesh is let go: its buffers and the pictures it
carried leave the GPU. Ten seconds, so a player turning back at a cell's edge
does not load it again. A mesh something else in the world still wears is
kept, a template in storage included, so a clone made by a script never waits
for its mesh. So is anything you hold with `api:ContentProvider.Keep` or
preloaded.

The frame report says "Let go of N meshes" and how much GPU memory went with
them. A world that does not stream lets go of a scene's content when the
scene is left, as before.

## Textures within a budget

`api:GraphicsService.TextureStreamingBudget` is the memory, in mebibytes, the
game's own textures may hold; nought is no limit. Over it, the textures worn
farthest from the camera are loaded again without their largest level, a
quarter of the memory each step and at most two steps, until they fit. A
texture worn within 40 meters is never made smaller, and neither is one no
material names, such as a decal's or a picture's. The copy drawn now stays
until the smaller one is up, so nothing goes blank.

The frame report says what the textures hold against the budget and how many
are kept smaller.

## Measuring it

- `--frame-report=1` prints a report a second with the lines above.
- `--no-occlusion-culling` draws everything, for a comparison; and
  `--occlusion-culling-always` tests every frame, never resting.
- `--lod-fade=always` fades every change of level, even in a headless run, a
  frame a sixtieth of a second, so `--screenshot-every=1` captures one frame
  by frame.
