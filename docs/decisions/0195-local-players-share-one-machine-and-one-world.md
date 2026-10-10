# 0195 — Local players share one machine and one world

- Status: accepted
- Date: 2026-10-09
- Decided by: the owner, who tested it in his game and opened the phase as
  built ("they are basically finished; if a change is needed we change it
  later"); written by the agent from the code, after the fact
- Builds on: ADR 0029 (input contexts and actions), ADR 0189 (preferred
  input, gamepad family), ADR 0192 (gameplay actions and UI navigation share
  device input), `docs/briefs/hordewake-local-coop-2026-10-09.md`

## Why this is written after the code

Several players on one machine arrived in the tree on 2026-10-09 with a
brief and no decision record: ADRs 0189 and 0192, which it leans on, do not
mention a second player. This is that record, taken from what the code does,
with what it does not do said as plainly -- so the next change to it is a
change to a decision and not to an accident.

## Decision

**A local player is a real `Player`, added by the game, in a world that is
not networked.** The engine gives a game the pieces -- a second, third and
fourth `Player`, each with its own simulation input, and remote events that
say which of them spoke -- and decides nothing about seats, cameras or the
screen.

### Who they are

`NetworkService:AddLocalPlayer()` makes a `Player` under `NetworkService`
and fires `PlayerAdded`; `RemoveLocalPlayer(player)` fires `PlayerRemoving`;
`GetLocalPlayers()` lists them in the order they joined, the primary first.

A guest is a `Player` like any other: `PlayerComponent.local` is true for it
as for the primary, and the primary is simply the first local player
present. It has a session `UserId` from `0x80000000` up and the name
`Player<userId>`. No character is made for it: `Player.Character` is the
game's to set, as it is for the primary.

### Only where nothing is on the wire

`AddLocalPlayer` answers `nil` unless the topology is `Solo`, a primary
exists and no host or join is pending -- as a host, a dedicated server or a
replica it is `nil` and not an error. And `Host` and `Join` raise
(`scene.err.network_local_guests`) while a guest exists. **A match is either
several players on one machine or several machines, never both.** Nothing of
a guest is in the protocol, which is why it did not move.

That is a limit chosen, not an oversight: a guest on a networked client needs
a second identity on the server, input for two players in one client's
stream, and prediction for two characters. None of that is built.

### Whose input

By the game's own hand, through the contexts it already uses:

- `InputContext.Player` -- the local player whose simulation intents the
  context's actions become. `nil` is the primary.
- `InputContext.GamepadId` -- the one controller the context reads; 0 is
  every controller, as before. Keyboard and pointer bindings are not
  filtered by it: they go to the context's `Player`.
- `UIService.GamepadId` -- the one controller that navigates the interface;
  0 is every controller.
- `InputService:GetGamepads()`, `IsGamepadKeyDown`, `GamepadConnected` and
  `GamepadDisconnected`, and `InputObject.GamepadId`, to know which
  controllers there are.

Connecting a controller assigns nothing. A game that wants "press a button
to join" listens for the button and builds the context.

Each tick, every local player's intents are cleared and each action is
routed to its context's player. `RunService:BindToIntent(name, fn, player)`
binds a predicted action for one of them.

### Who said it

In a world with no wire a remote event still crosses from the client's side
of a game's code to its server's. `FireServerFor(player, ...)` says which
local player is speaking, and the server's handler receives that player as
it would a remote one; it raises for a player who is not local, or is
leaving. `FireClient(guest, ...)` arrives as `LocalClientReceived(guest,
...)`, so the client's code for a guest is told apart from the primary's,
whose messages stay on `ClientReceived`. `FireAllClients` arrives once, not
once a guest.

### Determinism

A guest is in the world's hash: its `UserId`, that it is local, and its
intents. An `InputContext` at its defaults -- the primary's, every
controller -- is quiet in the hash (D603), so a world with no second player
hashes as it did.

## What this does not do

- **No split screen and no camera a player.** The engine draws one picture
  from one camera; a shared screen and where its camera sits are the game's.
- **No interface focus a player.** One controller navigates at a time
  (`UIService.GamepadId`); a card each player picks from is the game's code.
- **No online play with guests**, as above.
- **No limit of its own.** A game chooses how many seats it has.
- **A replay does not record which controller.** The recorder writes keys and
  axes, not per-controller state, so in a replay a context that reads one
  controller reads nothing. A comment in `input.h` says replays carry each
  controller; they do not yet. Until they do, a scenario with a second player
  cannot be a determinism trace.

## Tested, and not

Tested: a guest added and removed, its `UserId`, a forged or foreign player
refused, `Host` and `Join` refused while one exists, intents routed to each
player, `BindToIntent` for a player, `LocalClientReceived`, `nil` on a
replica (`service_tests.cpp`); one controller's sticks and sinking isolated
while the aggregate still works, four controllers at once, one unplugged
while another is held (`input_tests.cpp`); menu navigation belonging to its
controller (`ui_navigation_tests.cpp`).

Not tested: `AddLocalPlayer` as a host or a dedicated server; the controller
list and its signals from a script; the unreliable remote's variants;
`FireAllClients` with guests; guests across a change of scene; the world's
hash with a guest in it. On hardware, two controllers moving two players is
confirmed by the owner; three and four are not.

## Owed

1. Per-controller state in the replay recording, and then a determinism
   scenario with two players.
2. The tests listed above.
3. The comment in `input.h` made true or removed.
