# 0126 — A part can be clicked, prompted and dragged without code

- Status: accepted (to be built; see `docs/briefs/toolkit-kickoff.md`, F1)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"isso aqui é certeza que precisamos:
  ClickDetector"*, and approving a proximity prompt and a drag detector from a
  survey of what the platform whose API shape this engine follows offers and
  this engine lacks. The survey read public documentation only (R7).
- Relates to: the input system (`InputAction`, `InputBinding`, `InputContext`),
  [0099](0099-teams-and-network-ownership.md) (network ownership),
  [0077](0077-game-messages-cross-the-wire-through-a-remote-event.md) (messages
  across the wire).

## Context

A part a player can press, open or pull needs, today, a raycast from the pointer
each frame, a distance check, a hover state, touch handling, and in a match a
remote message to the authority that the authority must validate. Every game
writes it; most write it with a bug. Three instances cover almost every case.

## Decision

### 1. `ClickDetector`

- Child of a part or a model. `MaxActivationDistance` (default 32 m),
  `CursorIcon` (an image, optional).
- Signals: `MouseClick(player)`, `RightMouseClick(player)`,
  `MouseHoverEnter(player)`, `MouseHoverLeave(player)`.
- The pointer (mouse, or a tap on touch) is raycast into the world once per
  frame by the engine; the nearest part with a detector in range wins; parts
  with `CanQuery = false` are skipped as a raycast skips them.
- **In a match** the click is sent to the authority, which re-checks distance
  from the player's character before firing `MouseClick` there with the player;
  the clicking machine fires it locally as well, so a client script can react at
  once. Hover is local only.

### 2. `ProximityPrompt`

- Child of a part, an attachment or a model. `ActionText`, `ObjectText`,
  `KeyboardKeyCode` (default `E`), `GamepadKeyCode` (default `ButtonX`),
  `HoldDuration` (0 = a press), `MaxActivationDistance` (default 10 m),
  `RequiresLineOfSight`, `Exclusivity` (`OnePerButton`, `OneGlobally`,
  `AlwaysShow`), `UIOffset`, `Style` (`Default` or `Custom`), `Enabled`.
- Signals: `Triggered(player)`, `TriggerEnded(player)`,
  `PromptButtonHoldBegan(player)`, `PromptButtonHoldEnded(player)`,
  `PromptShown(inputType)`, `PromptHidden`.
- `ProximityPromptService`: `Enabled`, `MaxPromptsVisible`, and the service-wide
  `PromptTriggered(prompt, player)`.
- **The default look** is drawn by the engine (a key or button glyph, the two
  texts, a hold ring), themed and i18n'd; on touch the prompt is tapped. `Custom`
  draws nothing and leaves `PromptShown`/`PromptHidden` to the game.
- The keys are bound through an `InputContext` the engine owns, so a game's own
  contexts can take priority over them.
- Distance and line of sight are measured from the local character's root
  (`CharacterBody`), or the camera when there is none.
- **In a match**: as the click — triggered locally, validated and fired on the
  authority.

### 3. `DragDetector`

- Child of a part or model. `DragStyle` (`TranslateLine`, `TranslatePlane`,
  `TranslateViewPlane`, `RotateAxis`, `RotateTrackball`, `Scriptable`), `Axis`,
  `ReferenceInstance`, `MinDragTranslation`/`MaxDragTranslation`,
  `MinDragAngle`/`MaxDragAngle`, `ResponseStyle` (`Geometric` moves the part
  directly, `Physical` pulls it with a force under `MaxForce`, `MaxTorque` and
  `Responsiveness`, `Custom` only reports), `MaxActivationDistance`,
  `CursorIcon`, `Enabled`.
- Signals: `DragStart(player, ...)`, `DragContinue(player, ...)`,
  `DragEnd(player)`.
- **In a match**: a `Physical` drag of an unanchored part gives the dragging
  player network ownership for its duration (ADR 0099); a `Geometric` drag is
  applied by the authority from the player's requests.

### 4. Around them

- Each is deterministic where it touches the simulation: a click, a trigger or a
  drag step is an input, recorded by the replay harness and applied on the tick.
- The editor shows them in the Explorer with their icons and draws each one's
  activation distance when selected.

## Consequences

- A door that opens, a lever that is pulled, a button that is pressed — with no
  raycast in the game's code, and correct on touch and in a match.

## Not decided here

- A detector for 2D (`Part2D`). The same classes may take it later.
