# The level-up roadmap: nine categories, in order

The owner, on 2026-10-10, set the engine's next work as nine categories and
their order ("nessa ordem mesmo"), relayed by the orchestrating session and
confirmed by him the same day. This is the ledger: what each category is,
the stages it is expected to take, and where it stands.

## How it is run

- **The unit is the category**, not the stage (the owner, 2026-10-10: the
  work was going in pieces too small, and too much of the time to gates and
  CI). A whole category is built -- or, where one is truly too large, a half
  named in advance -- before the full gate. While building: the inner loop
  only, the build and the tests of the module touched. At the end: the
  determinism test, the full gate once, one push, one package, one report.
  What the gate finds is fixed in the same batch and only the lane that
  failed is run again. CI is read after the push and not waited on; a red
  one is fixed at the head of the next batch, and `main` moves only to a
  green commit.
- **What exists is read first.** The list below was named from what an
  engine of this class has, not from this code. Before a category starts,
  what the engine already does in it is read and written down here, under
  *What is there*; anything already built becomes an amendment, not a new
  feature.
- **One ADR a feature**, the ADRs of a category written together at its
  start and sent for approval in one message; the code does not wait for the
  answer. Each says which side of the world's hash its feature is on (R10)
  and names any dependency with its licence and pin (R5, R6: permissive
  only).
- **Each category leaves** an example under `examples/`, a manual page, and
  its tests. A defect found on the way is fixed in the batch that found it,
  with its test failing first.

## Before the first category

One short batch, together with the last stage of the shape keys (ADR 0196):

| Item | State |
| --- | --- |
| Shape keys: the editor's sliders, `examples/36-face`, the measurements | built |
| A start that fails leaves a trace (D608) | built |
| A recording carries each controller; ADR 0195's owed tests | built |
| The 2 GB phone (D609): the interface's pictures keep to a budget, and `TextureQuality` is applied and starts at the machine's memory | built |

**Left for its category, and written down so it is not found again**: a
budget for the world's own textures. `GraphicsService.TextureStreamingBudget`
is kept and read by nobody; nothing releases a texture or a mesh when the
scene that used it is left or its chunk is streamed out
(`MeshLoader::forget` is called by hot reload alone). That is category 7's
(the world), beside occlusion culling; what a change of scene leaves on the
card is category 2's audit (e).

## 1. Animation

A character that moves like one without game code doing the blending.

1. **A state machine with blending**: states that are clips or blends of
   clips (a walk into a run by speed), transitions with a time and a
   condition, layers (a body and an arm), authored as an asset and driven by
   parameters a script sets.
2. **Inverse kinematics**: a foot on uneven ground, a hand on a weapon, a
   head looking at a target -- two-bone and look-at solvers applied after
   the pose, with a weight.
3. **Retargeting**: one clip on several skeletons whose proportions differ,
   where today a clip from another file is mapped by joint name alone.

*What is there*: `AnimationPlayer` and `AnimationTrack` with weights and
fades, clips from another file by joint name, `Bone` for attachments and
turns, ragdolls, `SpringBone` for secondary motion, shape keys. To be read
in full before the category's ADRs.

*To decide in the ADRs*: which of these drive what is SIMULATED. A pose
feeds sockets, hitboxes and ragdolls today, so a state machine's blend is on
the simulation's side of the hash; a foot planted by IK may be picture only.

## 2. Graphics: indirect light

Interiors and shadowed sides that are not flat.

1. **Reflection probes**: captured or baked cubemaps, placed and blended, in
   place of the one sky environment everything reflects today.
2. **A simple global illumination**: baked, or by probes -- the ADR
   recommends one, with what each costs a phone.

Folded in from the old queue: decals without the mask drawn again (audit c),
the prepass on a handheld and runs of two (audit d), what a change of scene
uploads and compiles (audit e).

*To be read first*: what ambient occlusion, contact shadows and image-based
lighting already do, and whether anything like screen-space reflections
exists.

## 3. Cinematics

1. **A sequencer**: a timeline with tracks for a camera, an animation, a
   sound and an event, authored in the editor and played from a script.

## 4. Editor

1. **A visual profiler** inside it: frame by frame, by system and by script.
2. **A node editor for materials**, on top of the surface shaders.

Folded in: an export that reuses the import cache (audit b), meshes compiled
side by side (audit f), the rest of the settings work (3c).

*To be read first*: what `--frame-stats`, the frame report and the Stats
panel already show.

## 5. Physics

1. **A vehicle**: wheels and a suspension.
2. **Mesh cloth**: the second tier of ADR 0194, on its colliders and dials.
3. **Destruction**.

## 6. Audio

1. **Occlusion**: a sound muffled behind a wall.
2. **Voice chat** for multiplayer.

## 7. World

1. **Levels of detail made automatically**.
2. **Occlusion culling** of what is hidden.

*To be read first*: the compiler already makes a chain of levels for a mesh
(`asset/mesh_format.h`) and the renderer chooses among them; what is missing
is to be established before this is called a feature.

## 8. Platforms

1. **The web**: a game that opens from a link. Opened by the owner on
   2026-10-10 (R15).
2. iOS follows the web and is **not open**: it needs the owner's Mac and his
   Apple account.

Folded in: G19, G20's fallback fonts, mobile M2 and M3, the gamepad proof.

## 9. Ecosystem

1. **A package manager** for sharing Luau modules and assets.

## What jumps the line

D596 -- a Windows 10 player's Direct3D 12 -- when its log arrives.
