# 0157 — Scripts run in the predicted step, and intents are written by code

- Status: accepted
- Date: 2026-10-03
- Decided by: the agent, on findings G37 and G38 of the test games
  (ludwerk-08, 2026-10-03), under the standing rule to decide as professional
  engines do and record it
- Amends: 0076 (a replica predicts its own character) and 0133 (the island
  stepped again). What they decided stands; this adds what a script does in the
  steps they take.
- Protocol: 37

## Context

A replica predicts its own `CharacterBody` (ADR 0076): its machine moves it at
once, and when the authority answers otherwise it is put where the authority
had it and every tick not yet answered is stepped again (ADR 0133). What is
stepped again is the physics: the island, and the command each tick gave the
controller.

Scripts are not. A dash written in `Heartbeat` -- "for a second after the key,
move fast" -- runs once on each machine, at each machine's moment. The
authority applies the replica's input a few ticks after the replica did, the
two dashes start at different ticks, and the replica is corrected at the start
and at the end of every dash: the rubber band the FPS test game showed at any
ping above zero (G37). And a correction in the middle of a dash restored the
character and stepped it again with the dash's moves baked into the recorded
commands, whether or not the dash was still on.

Second, input a script works out -- a turn from the mouse, a gesture, a bot --
had no way into the player's intents, which are what the authority applies and
what a prediction steps with; only an `InputAction` became an intent (G38).
And every intent carried its action's name as text, every tick, four ticks to
a message.

## How mature engines do it

- **Unity Netcode for Entities**: systems in the `PredictedSimulationSystemGroup`
  run for every predicted tick, again on every rollback; input is an
  `IInputComponentData` buffer per tick; predicted state is `[GhostField]`s
  restored before the re-run.
- **Photon Fusion**: `FixedUpdateNetwork` runs per tick and again on
  resimulation, with `Runner.IsResimulation`; `GetInput` answers the tick's
  input; `[Networked]` properties are rolled back.
- **Unreal** (Network Prediction, Mover): a simulation tick takes an input
  command and a sync state and is re-run on correction; auxiliary state rides
  beside it.

The common shape: game code that moves a predicted thing runs inside the
simulation tick, from that tick's input, and is run again when the tick is;
the state it keeps across ticks is declared, restored, and corrected by the
server; anything only cosmetic asks whether it is a re-run.

## Decision

1. **`RunService:BindToPredictedStep(name, fn)`** runs `fn(step)` inside the
   physics step, before it solves, once for every player's character this
   machine steps: on the authority all of them, on a replica its own. On a
   replica it runs again for every tick stepped again after a correction, with
   that tick's input. In a game played alone it runs once a tick. A `Bind`
   function and not a signal, because it is synchronous (R8): the engine's
   signals are deferred.

2. **The step is per character.** `step.Character`, `step.Player`,
   `step.Tick`, `step.DeltaTime`, `step.Replaying`, `step.Random`,
   `step:GetIntent(action)`, `step:Pressed(action)`. **`Tick` is the player's
   own tick** -- the number their machine gave the input -- so a dash that ends
   at `Tick + 60` ends at the same tick on both ends, whatever the latency.
   `Pressed` is the button going down between this step and the player's last.
   `Random` is seeded by the player and the tick. A tick the authority holds a
   player's input for (its delay adapting) is not stepped twice.

3. **`BasePart:BindToPredictedTouch(fn)`** runs `fn(character, step)` inside
   the step when a stepped character begins touching the part -- again in a
   replay. The island remembered for a replay now keeps what each character
   was touching, so a touch begun after the restored tick is begun again and
   one already in progress is not. Nothing runs while nobody touches.

4. **Predicted state is what the step writes.** An attribute a predicted
   step or touch writes on its character is predicted: remembered with the
   island every tick, put back before a replay, and sent by the authority in
   each snapshot to that character's replica (protocol 37), which compares it
   with what it remembers at the answered tick. A difference is a correction,
   like a difference of place, and the replay starts from the authority's
   values. The `Attributes` copy of them is not applied to the replica's own
   character -- it is older than the prediction. No list is declared: what the
   step writes is what is predicted, so the two ends cannot disagree about the
   list. A replica whose own code writes one of them is put right at the next
   answer (the hacked-client case of the acceptance).

5. **A replay steps with what the code outside the step told the character.**
   The command recorded for a tick is the move and jump before the predicted
   step ran; the step runs again in the replay and writes its own. A dash
   undone by a correction is not walked anyway.

6. **A predicted function may not wait.** It is called, not resumed: `task.wait`
   in it raises. What it would do after a wait is not part of any tick.

7. **`RunService:BindToIntent(name, fn)`** runs `fn(intent)` every tick right
   after this machine's input becomes its player's intents; `intent:Set(action,
   value)` writes one -- a boolean, a number, a `Vector2` or a `vector` -- over
   an `InputAction` of the same name. The authority receives it as any intent.

8. **Intents cross by number** (protocol 37). A replica sends each action's
   name once a connection, reliably, in an `IntentNames` message; intents carry
   a 16-bit number. A button not held is left out (it reads as an action not
   sent, false); other defaults are not, since they would read as false where
   a number was meant. An intent that arrives before its name -- the two travel
   on different channels -- waits for it rather than being lost with its press.
   A name once given stands for the connection.

## Consequences

- A dash and a stamp's jump pad written this way run at 165 ms round trip
  without a correction; a correction in the middle of the dash is one
  correction, the dash continuing; a client that writes its own dash end is
  pulled back once (`network_session_tests.cpp`, G37's acceptance).
- A predicted function costs a Luau call per character per tick on the
  authority, and per replayed tick on a replica. Touch functions cost nothing
  until something touches.
- Bindings, intent writers included, go with the scene of the script that made
  them.
- Not here: a part other than a character stepped by scripts in the predicted
  step (an owned kart is ADR 0099's owner simulation); predicted attributes on
  anything but the character.
