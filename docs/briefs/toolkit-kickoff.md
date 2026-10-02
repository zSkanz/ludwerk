# The toolkit: the kickoff and the ledger

Block F of [`game-ready-plan.md`](game-ready-plan.md), approved by the owner on
2026-09-27 from a survey of what the platform whose API shape this engine
follows offers and this engine lacks (public documentation only, R7). The owner
chose from the survey and **left out**, on purpose: `SpawnLocation`, `Seat` and
`VehicleSeat` ("not now"), `Tool` and `Backpack`, and `Explosion`. Do not build
them from this ledger.

The order across blocks is in [`game-ready-plan.md`](game-ready-plan.md); this
file is the order of work inside block F and where each piece stands.

| Stage | Decision |
|---|---|
| F1 — click, prompt, drag | [ADR 0126](../decisions/0126-a-part-can-be-clicked-prompted-and-dragged-without-code.md) |
| F2 — movers and constraints | [ADR 0127](../decisions/0127-movers-and-constraints-move-a-part-without-code.md) |
| F3 — UI layouts, adaptation, gamepad | [ADR 0128](../decisions/0128-ui-lays-out-in-grids-and-pages-adapts-to-any-screen-and-is-driven-by-a-gamepad.md) |
| F4 — Highlight, Beam, Trail | [ADR 0129](../decisions/0129-highlight-beam-and-trail.md) |
| F5 — solids and shapes | [ADR 0130](../decisions/0130-parts-combine-into-solids-and-two-more-shapes.md) (Manifold adopted, 2026-09-30) |
| F6 — sound effects and vibration | [ADR 0131](../decisions/0131-sound-effects-vibration-and-preloading.md) §1-2 |
| F7 — text chat | [ADR 0132](../decisions/0132-players-chat-through-textchatservice.md) |
| F8 — preloading | [ADR 0131](../decisions/0131-sound-effects-vibration-and-preloading.md) §3 |

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- The rules of [`game-ready-plan.md`](game-ready-plan.md).
- Every new class has an icon in the theme, a reference page, i18n'd strings for
  anything a player sees, and a place in the editor's *Insert* menu.
- Anything that touches the simulation (F1's inputs, F2) is in the trace and the
  rollback snapshot, and reproduces across worker counts.

## Stage F1 — click, prompt, drag (ADR 0126)

- [x] `ClickDetector`: pointer and touch raycast once a **tick** (not a frame:
  from the camera and the viewport as the tick begins, so the answer is a
  function of the input and replays); hover; the four signals;
  `MaxActivationDistance`. **`CursorIcon` is stored and not drawn** (`Inert`).
- [x] `ProximityPrompt` and `ProximityPromptService`: the properties and signals
  of ADR 0126 §2; the default look (the key, the texts, a hold bar filling
  under the key), drawn into the screen's draw list above the game's UI;
  `Custom` style; tap on touch; the keys below every game context (a key a
  game's context sank, or the interface took, is not the prompt's --
  `InputSystem::consumed`). `GamepadKeyCode` defaults to `ButtonWest`: the
  enum has no `ButtonX`.
- [x] `DragDetector`: the six drag styles, limits from where the part rested,
  `Geometric`/`Physical`/`Custom` responses, the three signals with the
  pointer's ray. A `Physical` turn is put there (`MaxTorque` inert: no angular
  impulse yet).
- [x] In a match: local fire, authority validation (reach from the
  character, with a part's size and a slack of 4 m; enabled), fire on the
  authority with the connection's player -- `DetectorInput`, protocol 21 --
  and all three classes replicate. A drag crosses as rays (`DragInput`,
  protocol 26) and the authority moves it; a `Physical` drag of an unanchored
  part is the player's while it lasts. **Not checked on the authority: line of
  sight** (a prompt's reach is).
- [x] Editor: activation distances drawn when selected (the detector, or the
  part holding it).
- [x] An example: `examples/30-interactions`, a room with a button, a door
  opened by a held prompt, a drawer and a lever.
- [x] Tests: a click out of range does not fire and one in range does, with
  hover; a held prompt fires after `HoldDuration` and ends on release, and the
  service sees it; two prompts on one key show one; both classes replicate and
  a client's click and trigger reach the authority with its player
  (`session_tests`); a forged click from too far is refused.

## Stage F2 — movers and constraints (ADR 0127)

- [x] `LinearVelocity`, `AngularVelocity`, `AlignPosition`, `AlignOrientation`,
  `VectorForce`, `Torque`, evaluated in the physics step
  (`engine/scene/src/physics_movers.cpp`).
- [x] `HingeConstraint.ActuatorType` with motor and servo.
- [x] `PrismaticConstraint`, `RopeConstraint` (with winch), `RodConstraint`,
  `SpringConstraint`, `NoCollisionConstraint`.
- [x] State in the trace and the rollback snapshot; stable evaluation order.
  There is no mover state: each is a function of the world as the step finds
  it. A winch's rope is the `Length` property; a written velocity not yet
  handed to the solver is one bit of the snapshot.
- [x] `Visible` drawing in the editor and F3; ropes and rods drawable in a game.
- [x] Manual recipes: spinner, hovering pet, lift, rope bridge
  (`docs/manual/physics/movers.md`).
- [x] Tests: a motor reaches its speed under its torque cap; a servo settles at
  its angle; `AlignPosition` holds against gravity within `MaxForce` and sags
  past it; a rope never exceeds its length; the traces reproduce
  (`tests/conformance/physics/movers.spec.luau`, 51 cases against the real
  solver; `constraint_tests.cpp`; "ADR 0127:" in `world_host_tests.cpp`).
- [x] The amendments of 2026-10-02 (the ADR's last section): A1 a ball socket
  holds a pose; A2 reaction force and torque; A3 `Stiffness` and `Damping`; N1
  `Mass` and `AssemblyMass`; N2 `Collided`; N3 `BreakForce`, `BreakTorque`,
  `Broken`, `GetForce`, `GetTorque`; N4 damping; N5 writable velocity; N6
  `GetBodiesInSphere`.
- [ ] **Movers and joints on a body a replica predicts or owns.** They are not
  on the wire (the authority simulates them), so a part a player's machine
  predicts is not pushed by a mover there. Needs `Attachment` and these classes
  to travel; built with the network's ledgers.

## Stage F3 — UI layouts, adaptation, gamepad (ADR 0128)

- [ ] `UIGridLayout`, `UIPageLayout`, flex in `UIListLayout` and `UIFlexItem`.
- [ ] `UIScale`, `UIAspectRatioConstraint`, `UISizeConstraint`,
  `UITextSizeConstraint`.
- [ ] `CanvasGroup` through the view texture registry, redrawn when dirty.
- [ ] `UIDragDetector`.
- [ ] Selection: `Selectable`, `NextSelection*`, `UIService.SelectedObject`,
  `AutoSelect`, `SelectionChanged`; d-pad, stick and arrows; the default look.
- [ ] The UI appearance gate gains cases for each.
- [ ] An example: an inventory grid, a paged tutorial, and a menu driven by a
  gamepad, at three screen sizes.
- [ ] Tests: layouts against hand-computed positions; constraints clamp; a
  `CanvasGroup` at 0.5 draws its children as one; selection moves to the nearest
  element in each direction.

## Stage F4 — Highlight, Beam, Trail (ADR 0129)

- [x] `Highlight` (mask, outline, fill; `DepthMode`; budget).
- [x] `Beam` and `Trail` as ribbons in the transparent pass.
- [x] Reference screenshots for each; F3 counts. As probes rather than
  checked-in pictures: `effects_gate` renders `tests/screenshots/effects` on a
  real device and asks what colour each place is.
- [x] Tests: an occluded highlight with `AlwaysOnTop` shows (`effects_gate`);
  a trail's length follows `Lifetime` (`ribbons_tests.cpp`); nothing enters
  the trace (every property is presentation, and no determinism trace moved).

## Stage F5 — solids and shapes (ADR 0130)

- [x] **Manifold is approved** (R5): adopted on 2026-09-30 under the owner's
  standing rule -- Apache-2.0, inside R6 -- and recorded in ADR 0130.
- [ ] Vendor Manifold (manifest row, notices).
- [ ] `UnionAsync`, `SubtractAsync`, `IntersectAsync`; `UnionOperation`,
  `NegateOperation`; sources kept; Separate.
- [ ] Editor: Union, Negate, Separate on the selection, with undo.
- [ ] `CollisionFidelity` (`Box`, `Hull`, `Default`).
- [ ] `Enum.PartShape` gains `CornerWedge` and `Truss` (appended).
- [ ] Tests: a subtraction leaves a closed mesh; Separate restores the sources
  exactly; an anchored union collides as its mesh.

## Stage F6 — sound effects and vibration (ADR 0131 §1-2)

- [x] The nine effect classes -- in the engine's own mixer rather than on
  miniaudio's graph, which the mixer has never been (ADR 0131, as built): the
  vendored reverb, and the engine's own filters and five others.
- [x] `HapticService` over SDL3; stop on focus loss. The service, its rules
  and its tests are in.
- [ ] **On hardware** (D476): the vendored SDL is built with joystick, haptic
  and HID off, so no gamepad works and nothing vibrates. Turn them on, as a
  push of its own; then the Android vibrator on a phone.
- [x] Tests: an effect changes the rendered samples (an offline render compared
  against the dry one and against what the effect is for); vibration calls
  reach the device (a fake standing where SDL does).

## Stage F7 — text chat (ADR 0132)

- [ ] `TextChatService`, `TextChannel`, `TextChatMessage`, `TextChatCommand`.
- [ ] Default channels, team channels, whispers, commands.
- [ ] Through the authority: length, rate, membership, filter, `ShouldDeliver`.
- [ ] The default chat window, input bar and bubbles; configuration objects;
  themed and i18n'd.
- [ ] Tests: a message reaches its channel's members only; the rate limit drops
  a flood; a filter rewrites; a whisper reaches one player.

## Stage F8 — preloading (ADR 0131 §3)

- [x] `ContentProvider:PreloadAsync(items, callback)` and `RequestQueueSize`,
  on the warming path of A2c (ADR 0125): meshes and pictures through the mesh
  loader's warm lists (`warmMeshes`, `warmTextures`), a sound by opening it and
  reading its length, anything else by whether it resolves. An instance item
  is every `asset://` its properties and its descendants' name.
  `Enum.AssetFetchStatus` (`Success`, `Failure`) appended at the end.
  **Not yet:** ADR 0125's budget (`[scene] max_prepared_bytes`), which A2c did
  not build either.
- [x] Tests: the callback reports each item (a found mesh, a missing one, an
  instance's picture) and the call resumes once all have; `RequestQueueSize`
  reads 0 after; a bad item is a keyed error. Headless, where nothing draws, so
  "resident" is "resolves" there; a windowed run answers from the libraries.
- A new service moves every determinism trace at tick zero (the hash covers
  every instance): re-recorded with `engine-host --replay=tests/determinism
  --record-replay`.

## Findings

(Filled as the work goes: what the ADRs assumed that reality corrected.)
