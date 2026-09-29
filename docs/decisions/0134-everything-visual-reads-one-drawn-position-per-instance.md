# 0134 — Everything visual reads one drawn position per instance

- Status: accepted (built 2026-09-29; D253 to D255)
- Date: 2026-09-29
- Decided by: the owner, on 2026-09-29, after seeing the name over a character
  smear when it walked in a match. Told that the cause was a pattern and not one
  bug, he asked for what the large engines do: *"UMA resolução de 'posição de
  desenho neste frame' por instância … e TODO consumidor visual lê dela"*, with
  a rule test and a lint so nobody brings the problem back.
- Builds on: D047 (a frame is drawn between two ticks), the multiplayer
  smoothness brief (a correction's slide, D244),
  [0076](0076-replicas-predict-their-own-and-draw-the-rest-between-snapshots.md)
  (the rest of the world is drawn between snapshots),
  [0107](0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md) (views).

## Context

The simulation steps at 60 Hz and a display does not, so since M8 the renderer
draws each part between the last two ticks (`render::interpolatedCFrame`), and
since the smoothness brief a replica's own character slides off where a
correction put it. That covered parts, meshes, decals, lights on parts and the
main camera. **Everything else read the simulated `cframe`**, which moves a tick
at a time:

1. `BillboardGui` and `SurfaceGui` -- the name over a character, which smeared.
2. `ParticleEmitter` -- a moving emitter's particles were born ahead of it, in steps.
3. The cameras of `CameraTexture`, `ViewportFrame` and `SubWorld`.
4. `Part2D` -- not interpolated at all; every 2D game stepped above 60 Hz,
   the platformer on a 120 Hz phone included.
5. The pointer: `ClickDetector` hovered through the tick's camera, the editor
   picked the simulated place.
6. `Attachment.WorldCFrame`, the root of 2 and of the beams and trails to come.
7. `ProximityPrompt`'s box, anchored at the tick and projected through the
   drawn camera.

Measuring it found three more:

- **Other machines' parts were never drawn between ticks** (D255): the snapshot
  moved them before the history was captured, so their previous place was their
  current one and they stepped at 60 Hz.
- **The slide moved one part** (D254): welded limbs, accessories and the name
  over the corrected character stayed where the correction put the body.
- **Alpha zero drew the NEXT tick** (D253): zero answered "the tick itself", as
  no history does, so a frame landing exactly on a tick jumped ahead and the
  frame after it went back -- every other frame at 120 Hz.

## Decision

1. **`render::DrawPoses` resolves where each instance is drawn this frame**,
   once per instance, remembered for the frame: a part (between ticks, plus a
   correction's slide for the corrected part and everything under it), a camera,
   an attachment (where it sits on its part, carried by that part as drawn), a
   2D part (position and turn, the shorter way round). The frame begins it once,
   after the ticks, from the history and the frame's alpha.
2. **Everything visual asks it**: the extraction (`render::extract` takes it),
   world UI drawn and clicked, particles, the cameras of all three kinds of view
   (a sub-world keeps its own history), `ViewportFrame`'s cache key, prompts
   (drawn where their anchor is drawn; a tap is tested on the box drawn),
   the pointer's ray (`EngineState::drawnCamera`), the editor's pick, markers,
   selection boxes, skeletons and the physics wireframe.
3. **The history is captured before the snapshot, and a correction moves the
   corrected character's last place with it** (`app::runDrawnTick` and
   `app::receiveDrawn`, which the frame loop and the tests both run; the
   replica reports how far its corrections moved the character,
   `VisualCorrection::displaced`). Another machine's parts are drawn getting
   where the snapshot put them; the corrected character slides without the
   correction counted twice, on a frame with a tick or without one.
4. **No history is the tick; alpha zero with one is the tick before.** A world
   drawn at its tick -- headless, an editor holding it still -- passes none.
5. **Sound stays with the simulation.** The listener and emitters are where the
   tick left them: what is heard follows the simulation, and a tick of
   position is not audible. `docs/architecture.md` §3 says so.
6. **A lint holds it**: `tools/repo/drawcheck.luau`, in the Luau gate, fails a
   visual file that reads a raw `cframe` or `worldCFrame`; a read that is not a
   drawn position says why on its line with `// raw:`.

## What this touches

Asked by the owner before anything shipped: not only parts and attachments, the
whole engine, so that nothing else breaks. Every reader of a transform, of the
camera and of the frame's two clocks, with the file and what was decided.

**What this change does not do**, because most of the list turns on it: it
writes nothing. `DrawPoses` reads the simulation and the history; no simulated
property -- `Camera.CFrame` included -- is written by a frame because of it.

| Reader | Where | Decision |
|---|---|---|
| **The world hash** | `scene/src/world_hash.cpp` (the accessor walk, ~535) | Unchanged. `Camera.CFrame` is hashed as before, and nothing here writes it per frame. The three new fields -- `EngineState::drawnCamera`, and a `ShownPrompt`'s `drawnAt`, `drawn` and `hangsFrom` -- are not properties, so the hash never sees them. A camera a game writes itself at render rate (`RunService.PreRender`, as examples 24 and 30 do, or an `InputContext` with `Rate = Render`) is a write into hashed state on the frame's clock -- a fact of those games that predates this, and one that never happens without a window: `PreRender` and the render-rate dispatch run only in a windowed frame (`engine.cpp`, `WorldHost::preRender`), so no trace, replay or gate can see it. The determinism traces are unchanged; the gate compares them. |
| **Detectors** (`ClickDetector` hover and click, `ProximityPrompt` taps; `DragDetector` when it exists) | `script/src/detectors.cpp` (`View`, ~72) | The pointer's ray goes through the camera as the last frame drew it, and a prompt's tap is tested on the box drawn. Both are **input**, sampled once a frame with the pointer position they belong to -- the picture the player pointed at. Without a window there is none of either -- a headless client, networked or driven, included -- and the tick's camera and the prompt's projected anchor are used, exactly as before: a replay, a gate or a server is unchanged. A prompt not drawn this frame (a minimised window) is tested where the tick sees it. `DragDetector` will take the same ray. |
| **The network** | `replication/`, the per-peer checksum | Unchanged: nothing replicated is written. A replica's own character is still simulated from its intents; a camera is local and never on the wire; movement relative to the camera reads `Camera.CFrame`, the simulated one, on the tick, so what an intent says does not depend on the frame rate. The checksum covers what the snapshot reconstructs, which this does not touch. |
| **Streaming** | `app/src/streaming_host.cpp` (~268) | The simulated place, deliberately: what loads is simulation, and a tick of camera motion is far inside any load radius. |
| **Sound** | `audio/src/audio.cpp` (~712, ~774) | The simulated place: the listener and emitters are where the tick left them. What is heard follows the simulation, a tick of position is inaudible, and `docs/architecture.md` §3 says so. |
| **A camera a game writes on the frame's clock** | `engine.cpp` (`host->preRender`) | The frame's poses are resolved again after `PreRender` and the render-rate dispatch, so such a camera is drawn where it was written this frame -- as it was before this change -- and the drawn camera the pointer uses is taken then. The poses resolved before it serve only what is drawn before the scripts run (the physics wireframe and the editor's markers). |
| **Views** | `app/src/view_host.cpp`, `engine.cpp`, `world_host_sub_worlds.cpp` | `CameraTexture`, `ViewportFrame` and `SubWorld` cameras are drawn cameras; a `ViewportFrame`'s cache key is its drawn contents; a sub-world keeps its own history, captured on every one of this world's ticks whether it steps or not, and a paused world's sub-worlds are drawn at their tick. |
| **Attachments** | `render/src/draw_poses.cpp` | An attachment that is not a bone is its part as drawn times its own `CFrame` -- exact, where the `WorldCFrame` the physics resolved is a move behind a part a script moves on `Heartbeat`. A bone is its part as drawn times where the physics last put it on the rig. |
| **The 2D camera** | `render/src/render_world.cpp` | The same drawn camera, orthographic or not, and `Part2D` is now drawn between ticks too. Example 20's camera, eased on `Heartbeat`, was drawn between ticks while the hero it follows was not -- the hero shook against it; now both are. |
| **Picking and pivots** | `app/src/picking.cpp`, `scene/src/pivot.cpp` | Picking tests drawn poses. `pivot.cpp` is geometry for the editor's tools and for `GetPivot`/`PivotTo`, which are simulation: unchanged. |
| **`DrawPoses` itself** | `render/src/draw_poses.cpp` | The one file allowed to read raw transforms; `drawcheck.luau` exempts it by name. |
| **The editor** | `editor.cpp`, `inspector.cpp`, `debug_overlay.cpp` | Its own camera runs on the render clock and is an override, drawn where it is. While editing nothing ticks and the frame passes no history, so drawn is simulated. In Play the pick, markers, selection boxes and skeletons are drawn poses -- the pick the last frame's, the picture clicked, and only in Play, since a Stop replaces the world under them. The gizmo is shown only while editing. The inspector and the F3 overlay show property values, which are the simulation's by definition. |
| **Android** | the 120 Hz phone | Nothing runs per frame that did not before. A touch camera at `Rate = Render` is the game's input on the frame's clock, as ADR 0039 says; what this changes on the phone is that a 2D game is drawn between ticks, and that a frame landing on a tick no longer jumps (D253), which at 120 Hz was every other frame. |
| **Performance** | the frame | No Luau runs per frame because of this. `DrawPoses` resolves an instance once per frame on first use -- the same interpolation the extraction already did, plus a remembered answer; measured in `docs/perf-baselines.md`. |
| **Gates without a window, and reference images** | `render_world.cpp`, the lavapipe goldens | A world drawn at its tick passes no history and is drawn exactly as the tick left it, which is what headless runs always drew: the images and traces do not move. |
| **Examples and templates** that move the camera on `Heartbeat` | `examples/`, `templates/` | Unchanged: the camera is drawn between ticks as it was since M8, and now so is everything around it. |

**Reviewed before it shipped.** A focused audit of the change found one defect
-- the poses were resolved before `PreRender`, which would have drawn a camera
written on the frame's clock a frame late -- and five risks: a paused world's
sub-world swinging between two ticks, an attachment drawn a move behind, a
correction on a frame without a tick counted twice, a headless client pointing
through a drawn camera, and an editor pick on the frame of a Stop. Each is fixed
as the rows above say, and the rule test now measures every item against where
it is put on its part rather than against its own first frame, which a constant
lag would have passed.

## Consequences

- Rule tests: a part moving at 12 m/s with a `BillboardGui`, a `SurfaceGui`, an
  `Attachment`, a `ParticleEmitter` on it and a camera kept on it by a script,
  drawn at 144 Hz through a correction's slide: every item within a millimetre
  of its place on the part and a pixel of where the part as drawn puts it
  (`draw_poses_tests.cpp`); a `Part2D` at a steady speed moving the same
  distance every frame at 120 Hz; another machine's cart moving the same
  distance every 144 Hz frame, with a name over it that stays on it
  (`network_session_tests.cpp`). Put back to the simulated place, the billboard
  alone was 15 pixels and 0.9 m off.
- A click lands on the picture the player saw: through the camera as drawn.
  A clickable moving faster than its size in a tick is still hit where the tick
  has it; the ray is the drawn one.
- The blend a predicted part owes when it leaves the predicted set (ADR 0133)
  now has the slide it needs; it is the next use of it.
