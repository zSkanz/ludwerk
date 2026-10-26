# 0264 — What the world no longer needs is let go, and what is far is kept smaller

- Status: accepted
- Date: 2026-10-26
- Builds on: ADR 0075 (worlds stream in cells), ADR 0086 (cells are cubes),
  the scene sweep of `MeshLoader` (a scene left lets go of what the
  next does not name)

## The question

A world streamed in cells loads each cell's meshes and textures as the cell
comes in, and never lets them go while the scene is open: the sweep that
frees content runs when a scene is left, not when a cell is. A flight across
an open world therefore grows its GPU memory with every cell it passes, until
a phone or an integrated GPU runs out -- and `GraphicsService.TextureStreamingBudget`
is reported but does nothing.

## Decision

1. **Counted per chunk.** The streaming glue counts, for each resident chunk,
   the mesh contents its records name. When a chunk is evicted and a content
   is named by no resident chunk any more, it becomes a candidate.
2. **Let go when nothing names it.** A candidate is released -- mesh, its
   materials' textures, the cache's buffers -- once no instance anywhere in
   the world has named it for ten seconds: not only the Workspace, so a
   template in storage that a script clones keeps what it wears, and never
   what `ContentProvider:Keep` holds, a preload asked for, or what is on its
   way in. Ten seconds so a player turning back at a cell's edge does not
   load it again. What is named again before then stays.
3. **Textures kept smaller by distance.** `TextureStreamingBudget` (MiB, 0 is
   no limit) is applied to the textures of the project's compiled content:
   every two seconds, from where the camera is, the plan is made again. While
   they would hold more than the budget, the farthest -- by the nearest part
   whose material names them -- lose their largest level, a quarter of the
   memory each step, farthest first and a round at a time, at most two
   levels each. One worn within 40 metres is never made smaller, and neither
   is one no material names: a decal's, a particle's, a picture's. Those the
   plan no longer wants smaller are loaded whole again. Reloaded off the
   frame through the path texture quality already uses; the copy drawn now
   stays until the new one is up.
4. **Seen.** The frame report says how many meshes were let go since its
   last line and the GPU memory that went with them, and, where a budget is
   set, what the game's textures hold against it and how many are kept
   smaller.
5. **The same in a world that does not stream.** What no cell hands over is
   not a candidate: a scene's content is let go when the scene is left, as
   it was.

## Proof

- **The mesh flight** (`mesh_flight`, with the tests that measure time): a
  strip of 48 cells of 256 metres, every cell wearing a mesh no other does,
  flown end to end at 200 metres a second, paced at sixty frames a second.
  It lets go of 33 to 35 meshes, 0.7 MiB of GPU memory each, and the
  buffers on the card climb to 16 MiB over the first cells and stay between
  16.1 and 16.8 MiB to the end. Without letting go they would hold the whole
  strip, 33 MiB and more. The gate asks for 24 meshes let go and the end
  within 1.5 MiB of two fifths of the way.
- **The rules, each held by a test**: a mesh two cells wear is handed over
  when the second leaves, not the first; one a template under no Workspace
  wears is kept; one unnamed is let go ten seconds on, not before, asked
  every two seconds; the budget takes a level from the farthest worn first,
  a round at a time, never from one within reach or worn by nothing.
- The textures' reload is the path `TextureQuality` loads a smaller texture
  by, which is the one used on every phone; what is new is choosing which.
