# 49-far-city

**A city too far to see all of** (ADRs 0261 to 0264): an avenue ten kilometers
long, streamed in cells, flown down at street level for thirty seconds and
then from above, with the four things the engine does to draw a world that big
said by the frame report as they happen.

- **What a wall hides is not drawn** (ADR 0262). At street level the old
  town's blocks hide the towers behind them, and a ridge of terrain across the
  avenue hides what is past it. The "Occlusion culling" line says how many
  draws a frame were left out; from above, where little is behind anything,
  it says the culler rests -- testing one frame a second to see whether it
  would pay again.
- **What the world no longer needs is let go** (ADR 0264). Each cell's towers
  wear a mesh no other cell wears. The cells behind the camera leave, and ten
  seconds later their meshes leave the GPU: the "Let go of" line.
- **What is far is kept smaller** (ADR 0264). The old town's sixteen facades
  are more than the texture budget this example sets, three mebibytes; as the
  town falls behind, the farthest lose their largest level: the "Textures"
  line's count.
- **A mesh changes its level without being seen to** (ADR 0261). The towers
  change their levels of detail as the camera passes, fading over a quarter of
  a second where the quality allows: the "Levels of detail" line.

## Running it

The world is generated, not committed:
`lute examples/49-far-city/tools/generate.luau` writes it, and `run.bat`
does that the first time. Then `run.bat` flies it with the frame report on.

- `O` turns occlusion culling off and on (`GraphicsService.OcclusionCulling`).
- `T` turns the texture budget off and on
  (`GraphicsService.TextureStreamingBudget`).
- `V` steps the view distance through 0.5, 1 and 2
  (`GraphicsService.ViewDistance`).
- `Space` pauses the flight.
