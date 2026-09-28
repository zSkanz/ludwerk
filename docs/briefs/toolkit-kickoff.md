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
| F5 — solids and shapes | [ADR 0130](../decisions/0130-parts-combine-into-solids-and-two-more-shapes.md) (Manifold awaits approval) |
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

- [ ] `ClickDetector`: pointer and touch raycast once a frame; hover; the four
  signals; `MaxActivationDistance`, `CursorIcon`.
- [ ] `ProximityPrompt` and `ProximityPromptService`: the properties and signals
  of ADR 0126 §2; the default look (glyph, texts, hold ring), themed and i18n'd;
  `Custom` style; tap on touch; keys through an engine-owned `InputContext`.
- [ ] `DragDetector`: the drag styles, limits, `Geometric`/`Physical`/`Custom`
  responses, signals.
- [ ] In a match: local fire, authority validation (distance, line of sight),
  fire on the authority with the player; a `Physical` drag takes network
  ownership for its duration.
- [ ] Editor: activation distances drawn when selected.
- [ ] An example: a room with a button, a door opened by a prompt, a drawer and
  a lever dragged.
- [ ] Tests: a click out of range does not fire; a held prompt fires after
  `HoldDuration`; two prompts with `OnePerButton` show one; a client's click
  reaches the authority with its player; a forged click from too far is refused.

## Stage F2 — movers and constraints (ADR 0127)

- [ ] `LinearVelocity`, `AngularVelocity`, `AlignPosition`, `AlignOrientation`,
  `VectorForce`, `Torque`, evaluated in the physics step.
- [ ] `HingeConstraint.ActuatorType` with motor and servo.
- [ ] `PrismaticConstraint`, `RopeConstraint` (with winch), `RodConstraint`,
  `SpringConstraint`, `NoCollisionConstraint`.
- [ ] State in the trace and the rollback snapshot; stable evaluation order.
- [ ] `Visible` drawing in the editor and F3; ropes and rods drawable in a game.
- [ ] Manual recipes: spinner, hovering pet, lift, rope bridge.
- [ ] Tests: a motor reaches its speed under its torque cap; a servo settles at
  its angle; `AlignPosition` holds against gravity within `MaxForce` and sags
  past it; a rope never exceeds its length; the traces reproduce.

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

- [ ] `Highlight` (mask, outline, fill; `DepthMode`; budget).
- [ ] `Beam` and `Trail` as ribbons in the transparent pass.
- [ ] Reference screenshots for each; F3 counts.
- [ ] Tests: an occluded highlight with `AlwaysOnTop` shows; a trail's length
  follows `Lifetime`; nothing enters the trace.

## Stage F5 — solids and shapes (ADR 0130)

- [ ] **The owner approves Manifold** (R5). Nothing below starts before it.
- [ ] Vendor Manifold (manifest row, notices).
- [ ] `UnionAsync`, `SubtractAsync`, `IntersectAsync`; `UnionOperation`,
  `NegateOperation`; sources kept; Separate.
- [ ] Editor: Union, Negate, Separate on the selection, with undo.
- [ ] `CollisionFidelity` (`Box`, `Hull`, `Default`).
- [ ] `Enum.PartShape` gains `CornerWedge` and `Truss` (appended).
- [ ] Tests: a subtraction leaves a closed mesh; Separate restores the sources
  exactly; an anchored union collides as its mesh.

## Stage F6 — sound effects and vibration (ADR 0131 §1-2)

- [ ] The nine effect classes on miniaudio's graph (its delay, filter and shelf
  nodes, the vendored reverb node, and the engine's own four).
- [ ] `HapticService` over SDL3 and the Android vibrator; stop on focus loss.
- [ ] Tests: an effect changes the rendered samples (an offline render compared
  against a reference); vibration calls reach SDL (a fake device).

## Stage F7 — text chat (ADR 0132)

- [ ] `TextChatService`, `TextChannel`, `TextChatMessage`, `TextChatCommand`.
- [ ] Default channels, team channels, whispers, commands.
- [ ] Through the authority: length, rate, membership, filter, `ShouldDeliver`.
- [ ] The default chat window, input bar and bubbles; configuration objects;
  themed and i18n'd.
- [ ] Tests: a message reaches its channel's members only; the rate limit drops
  a flood; a filter rewrites; a whisper reaches one player.

## Stage F8 — preloading (ADR 0131 §3)

- [ ] `ContentProvider:PreloadAsync(items, callback)` and `RequestQueueSize`,
  on the warming path of A2c (ADR 0125).
- [ ] Tests: preloaded assets are resident; the callback reports each.

## Findings

(Filled as the work goes: what the ADRs assumed that reality corrected.)
