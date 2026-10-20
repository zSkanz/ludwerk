# 47-realms

**An online game of several worlds** (ADRs 0220 to 0222): a town, the fields
beyond it, and a dungeon made for each party that asks -- three processes or
more, one game, run by the manager from `servers.toml`.

- `servers.toml` names the worlds: `town`, where the front door sends every
  player, and `fields`; and the `dungeon` template, an instance at a time for
  one party, closed a minute after the last of them leaves.
- **The town has channels** (ADR 0230): when every channel open is nine
  tenths full, the manager opens another -- `town:2`, `town:3`, up to four --
  and a player going to the town goes to the fullest with room, a party all
  together. Emptied, a channel closes; long under a quarter full, its players
  are warned (`NetworkService.ChannelMerging`) and moved to another.
  `ludwerk server loadtest 127.0.0.1:7777 --bots=200` shows it.
- `src/scenes/town/server/Town.luau` has the road and the gate. The road is
  `NetworkService:Travel(player, "fields", data)`. The gate gathers everyone
  standing on it, asks for a dungeon of their own with
  `InstanceService:CreateInstanceAsync`, and sends them together with
  `TravelGroup`. `InstanceClosed` says when one is gone, and why.
- `src/scenes/fields/server/Fields.luau` reads what the town handed over in
  `TravelArrived`, and its road goes home.
- `src/scenes/dungeon/server/Dungeon.luau` is the instance: what the town
  gave it in `SceneService:GetLoadData()`, an exit back, and `Closing`.
- `src/client/Realms.luau` lives across worlds, under
  `GlobalScriptService.Client`: `@engine/travelscreen` covers the hop,
  `@engine/announcements` shows what the operator says, and the controls stay
  in `InputService` while the camera is made again in each world.
- `src/shared/Realm.luau` is what every world has: its ground, a character
  for each player, walking, the pads, and the chat (`@engine/chat`, ADR
  0239). Type in the box at the bottom left and press Return: a line goes to
  the world, `/l` to those near, `/p` to the party, `/w name` to one player
  in whatever world they are.

## Running it

- **In the editor**: open the folder, tick *This game's worlds* in Run and
  Debug, and Start match. The worlds run without windows, each with its own
  tab in the Console and a row in the Run view -- *End* on a row tries a fall,
  *Restart with this build* moves every world to the scripts as they are now.
- **From a terminal**: `run.bat` starts the manager and its worlds, with its
  console (`worlds`, `instances`, `town: status`); `run.bat play` is a player
  at their front door, `127.0.0.1:7777`, and each further `run.bat play`
  another. The servers sign their players in (`[accounts]` in
  `servers.toml`): a player first makes an account or signs in on the login
  screen (`src/client/Login.luau`), then joins. Tried from the editor, with
  Start match and "This game's worlds", each window is let in as a test
  account.
- `ludwerk server run` in this folder does what `run.bat` does, wherever the
  engine is installed.

Alone -- `run.bat solo`, or Play with one player -- the engine opens the town,
and its pads say they need the game's servers rather than go anywhere.

WASD or the left stick walks.
