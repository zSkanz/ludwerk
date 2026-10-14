# examples/23-surfaces — three surface shaders a project writes itself

A flag in the wind, a block that dissolves, and a pane of glass. Each is one
file in [`content/shaders/`](content/shaders/) written against
`engine/surface.hlsli` (ADR 0091), worn through a material in
[`content/materials/`](content/materials/). The engine builds every pipeline the
surface needs from it, so the sun, its shadows, the lights, fog and the post
chain apply as they do to anything else.

```
examples\23-surfaces\run.bat
```

| | shader | what it shows |
|---|---|---|
| the flag | `flag.surface.hlsl` | **vertex displacement** on something that is not water: a flat `engine://mesh/grid-64` on a pole, rippling away from it, its folds shaded from the same function |
| the dissolve | `dissolve.surface.hlsl` | a **Mask** surface cut by noise, with a glowing rim ahead of the cut. All three wear one material that lets a part change `Threshold` (`instanceParameters`), and the script moves each block's own with `SetMaterialParameter` |
| the glass | `glass.surface.hlsl` | a **blended** surface that reads the scene behind it (`readsSceneColor`) and bends it with `sceneColorAt` |
| the fourth block | `dissolve-graph.surfacegraph.json`, which writes `dissolve-graph.surface.hlsl` | **the same dissolve drawn as nodes** (ADR 0211): open the graph from Content and it is on the editor's canvas -- two noises mixed, a step for the cut, one line of code for the rim. Saving it writes the shader beside it, and the fourth block wears that through `dissolve-graph.material.json`, beside the three that wear the hand-written one |

**Edit any of the three while the editor runs** and it recompiles on save; the
first compile of each takes under a second, and every later run reads the
cache.

**The graph is one way.** It writes its shader each time it is saved, and the
first line of the written file says so: a change typed into
`dissolve-graph.surface.hlsl` is lost at the next save. To keep the surface as
code, delete the graph and the file is yours -- it is written to be read, a
statement a node, each local named after its node.

**What it does not do yet.** A dissolving block's shadow stays whole: the
shadow pass draws where a surface is, not where it has cut itself. And a
part's shader parameters are this machine's -- they do not replicate.
