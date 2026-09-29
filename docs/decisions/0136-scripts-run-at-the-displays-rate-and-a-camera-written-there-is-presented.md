# 0136 — Scripts run at the display's rate, and a camera written there is presented

- Status: accepted, built 2026-09-29
- Date: 2026-09-29
- Decided by: the owner, on 2026-09-29, relayed by another session: after ADR
  0134 made everything visual read the drawn position, *"um script só enxerga
  o CFrame simulado (60 Hz)"* -- the default camera turns on `Heartbeat`, so at
  144 or 240 Hz it answers the mouse 16 to 25 ms later than an engine that turns
  it every frame, and a camera ported from another engine that runs every frame
  shakes here. Five parts were approved, with the survey below written before
  any code, and one criterion: **no determinism trace moves**.
- Amends: [0134](0134-everything-visual-reads-one-drawn-position-per-instance.md)
  (its "A camera a game writes on the frame's clock" row), and
  `docs/api-design.md` §2.1 (the phases).
- Builds on: [0039](0039-input-context-rate-and-total-enums.md) (`InputContext.Rate`),
  [0124](0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md) (a handler belongs to the
  script that made it), D047 (why the default camera moved to `Heartbeat`).

## Context

A frame runs, in order: the ticks it owes; the poses of the frame
(`render::DrawPoses`, ADR 0134); the render-rate input dispatch (an
`InputContext` with `Rate = Render`); `RunService.PreRender`; the poses again;
the draw. A script in `PreRender` can move a camera every frame, but:

- it cannot read where anything is DRAWN -- `BasePart.CFrame` is the tick's --
  so a camera that follows a part at render rate follows it in 60 Hz steps, and
  the picture shakes (D047). The engine's own rig moved to `Heartbeat` for
  that, and inherited a tick of latency on the mouse;
- it writes `Camera.CFrame`, which the simulation reads (movement relative to
  the camera) and the world hash hashes, on the frame's clock. Nothing a replay
  or gate sees, since no render phase runs without a window (ADR 0134), but a
  write into simulated state at an arbitrary point between two ticks all the
  same;
- there is no ordering: two scripts that both move the camera on `PreRender`
  run in connection order, and a deferred signal cannot promise more.

## Decision

### 1. `GetRenderCFrame()`

`BasePart:GetRenderCFrame()`, `Attachment:GetRenderCFrame()` and
`Camera:GetRenderCFrame()` answer **where the instance is drawn this frame** --
`DrawPoses`' answer: between the last two ticks, a correction's slide, another
machine's parts behind their snapshots, the same teleport rule. **During a
render phase only**: a render step, a `PreRender` handler, an `InputContext`
action at `Rate = Render`. Anywhere else -- a simulation phase, a headless run,
a server -- it answers the simulated `CFrame` (an attachment's `WorldCFrame`),
because what a tick reads must not depend on when a frame happened (R10). In
the editor and `ludwerk dev` it warns once per script when called in a
simulation phase with a window, where it is almost certainly a mistake.

### 2. `RunService:BindToRenderStep(name, priority, fn)`

- `fn(dt)` runs **every drawn frame, in priority order** -- ties in the order
  they were bound -- after the render-rate input dispatch and **before**
  `PreRender`'s handlers, with the frame's `dt`. Called directly, in order,
  each on its own thread: a render step is a scheduled callback with an order,
  not a signal, so R8's deferral does not apply to it.
- `UnbindFromRenderStep(name)` removes it; binding a name already bound
  replaces it.
- `Enum.RenderPriority`: `First` 0, `Input` 100, `Camera` 200, `Character` 300,
  `Last` 2000 -- read as `Enum.RenderPriority.Camera.Value + 1`.
- Nothing runs without a window.
- **A binding belongs to the script that made it** (ADR 0124): it goes with its
  scene; a global script's stays.

### 3. A camera written in a render phase is presented

- `Camera.CFrame` written **in a render phase** goes to the camera's
  *presentation*, not to its simulated `CFrame`, and **is drawn exactly as
  written** -- not interpolated again. Read in a render phase, `CFrame` answers
  the presentation.
- **At the start of the next tick, a presentation written since the last one
  becomes the simulated `CFrame`**, before anything in the tick runs. The
  simulation reads the camera the player last saw, and only ever at a tick's
  boundary.
- `Camera.CFrame` written **in a simulation phase** is the simulated `CFrame`,
  as today, and ends the presentation: the camera is drawn between ticks again.
- **The world hash and the simulation read the simulated `CFrame` only.**
  Without a window no render phase runs, no presentation exists, and every run
  is what it was: **no trace moves**, and the gate is what says so.

| Where `Camera.CFrame` is read | What it answers |
|---|---|
| A render phase, with a presentation | the presentation |
| A render phase, without one | the simulated `CFrame` (`GetRenderCFrame` is the drawn one) |
| A simulation phase, the hash, a server | the simulated `CFrame` -- latched from the presentation at the tick's start |

### 4. The default camera turns every frame

`@engine/camera`'s rigs bind themselves to `BindToRenderStep` at
`RenderPriority.Camera`: each frame the rig follows its subject's
`GetRenderCFrame()` and writes the camera, which is presented. A rig given its
`TurnAction` and `LookAction` reads them there, from an `InputContext` at
`Rate = Render`, so a mouse moved turns the camera in the same frame.
`rig:Update` on `Heartbeat` still moves the camera **when no frame drives it**
-- headless, a gate, the flagship's autopilot -- exactly as it does today, so
the reference images and the soak do not move. `examples/10-open-world` goes
back to the render rate through the module, and D047's comment says the rule
that replaced it.

### 5. The pointer's ray

Already the drawn camera's on a client with a window (ADR 0134): a click's and
a drag's rays go through `EngineState::drawnCamera`, and a drag sends the ray
to the authority (protocol 26), which validates reach with the tick's camera.
The drawn camera is now the presentation where there is one, so the ray goes
through exactly the picture the player clicked. **The wire does not change.**

## What this touches

Surveyed before any code, as asked: the whole engine, so that nothing else
breaks.

| Reader | Where | Decision |
|---|---|---|
| **The world hash** | `scene/src/world_hash.cpp`, the accessor walk (~535) | It calls `Camera.CFrame`'s getter outside any render phase, so it hashes the simulated `CFrame`. The presentation is not a property. A windowed game's simulated camera now changes only at a tick's start, where it used to change whenever `PreRender` ran. No trace moves: no render phase runs without a window. |
| **Detectors** | `script/src/detectors.cpp` (`viewOf`, ~144; the rays at ~923, ~956) | Unchanged code. The client's rays go through `drawnCamera`, which is now the presentation where one exists; the authority validates reach from the tick's. No wire change. |
| **Streaming** | `app/src/streaming_host.cpp` | The simulated camera: what loads follows the simulation, and it is now the presented camera a tick late at most, well inside any load radius. |
| **Sound** | `audio/src/audio.cpp` | The simulated camera, as ADR 0134 decided: the listener is where the tick left it. |
| **Views** | `app/src/view_host.cpp`, `world_host_sub_worlds.cpp` | `CameraTexture`, `ViewportFrame` and `SubWorld` cameras are drawn through `DrawPoses::camera`, which answers a presentation first: a view camera moved in a render step is drawn as written. |
| **The 2D camera** | `render/src/render_world.cpp` | The same camera and the same rule; orthographic changes nothing. |
| **Picking, pivots** | `app/src/picking.cpp`, `scene/src/pivot.cpp` | Picking tests drawn poses and the drawn camera. `PivotTo` on a camera writes the simulated `CFrame` whatever the phase -- it is `PVInstance` geometry for the editor and for gameplay, which a render step has no business moving. |
| **`DrawPoses`** | `render/src/draw_poses.cpp` | `camera()` answers the presentation, exactly, before any interpolation. |
| **The editor** | `editor.cpp`, `engine.cpp` | Its own camera is an override and does not change. While editing no render phase runs a game's scripts (D061), so nothing is presented. In Play, a presented camera is what the viewport shows. The inspector shows the simulated `CFrame`. |
| **Multiplayer** | `replication/`, the per-peer checksum | A camera is local and never on the wire. Movement relative to the camera reads the simulated `CFrame` in the tick, now latched from what the player saw -- an input, sampled at the tick's start as every input is. The checksum covers what the snapshot rebuilds, which this does not touch. |
| **Android** | the 120 Hz phone | A touch look at `Rate = Render` turns the default camera in the frame it happened. Nothing else runs per frame that did not. |
| **Performance** | the frame | Luau runs once per render step per frame -- at 240 Hz, the default rig four times a tick. It is measured in the phase profiler (`PreRender`'s phase time) and recorded in `docs/perf-baselines.md`. |
| **Gates without a window, and reference images** | the headless runs, the lavapipe goldens | No render phase runs: no render step, no presentation, and `rig:Update` on `Heartbeat` moves the camera as it did. Images and traces do not move. |
| **Examples that move a camera on `Heartbeat`** | `examples/` | Unchanged: a camera written in a simulation phase is drawn between ticks, as since M8. |
| **Examples that move one on `PreRender`** | examples 24 and 30 | Their camera is now presented: drawn as written, and read by their `Heartbeat` code a tick later, at the tick's start. Unchanged to look at. |

## Consequences

- A camera follows what it looks at as it is drawn, turns with the mouse in the
  frame, and the simulation never sees a value written between two ticks.
- `Camera.CFrame` answers by phase. The table above is the whole rule, and
  `docs/manual` says it where a script author reads about cameras.
- Tests: synthetic frames at 240 Hz -- a mouse movement turns the default
  camera in the same frame; a character at 8 m/s, local and another machine's,
  followed by the default camera, stays within a pixel of the same place on
  screen, through a correction and a teleport (the camera and its subject jump
  together); `GetRenderCFrame` in a `Heartbeat` is `CFrame`; render steps run
  in priority order and stop when unbound; nothing runs without a window; no
  trace moves.
