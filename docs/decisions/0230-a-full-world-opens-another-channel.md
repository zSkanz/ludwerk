# 0230 — A full world opens another channel of itself

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0220 (the manager, `servers.toml`), ADR 0221 (instances),
  ADR 0222 (Travel and the pass), ADR 0229 (a player's budget)

## The question

A town holds as many players as one process can tell about each other. On a
launch day, or at an event, more come. The common answer is copies of the
crowded place -- channels, layers -- that players are spread across, that a
party shares, and that fold back together when the crowd goes.

## Decision

### 1. A world with channels

A `[[world]]` with `channels = true` is started as channel 1, and when every
open channel holds `channel_open_at` of its `max_players` (90% by default)
the manager starts channel 2 from the same scene, then 3, up to
`max_channels`. Each channel is a process of its own, as an instance is,
named `town` (channel 1) and `town:2`, `town:3`.

### 2. Where a player goes

`Travel(player, "town")` and the front door send a player to the channel
with the most players that has room -- crowds feel alive, and fewer channels
stay open -- unless they are travelling to somebody:

- **`TravelToPlayer(player, otherKey)`** sends them to the world and channel
  where that player is (a friend, the party's leader), when it has room.
- **`TravelGroup`** keeps a group in one channel, as it keeps it in one world.
- **`SwitchChannel(player, n)`** moves a player to channel `n` of the world
  they are in, for a menu.

### 3. Fewer when the crowd goes

A channel other than 1 that has been empty for `channel_close_seconds` (120
by default) is stopped. One that has held fewer than a quarter of its
`max_players` for `merge_minutes` (10 by default; 0 never) is merged: its
players are told (`NetworkService.ChannelMerging`, with the seconds), and a
minute later moved, a party at a time, to the fullest channel with room.

### 4. What a world knows

A world's script reads `NetworkService.Channel` (1 for a world without
channels) and asks for the list with `GetChannelsAsync`. Channels share
everything a world shares across the game: the same `CrossWorldService`
topics, the same saves, the same bans.

## Script surface

```toml
# servers.toml
[[world]]
name = "town"
scene = "scenes/town.scene.json"
entry = true
max_players = 150
channels = true
max_channels = 8
channel_open_at = 0.9        # of max_players, in every open channel
channel_close_seconds = 120
merge_minutes = 10
```

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

print(`this is channel {NetworkService.Channel}`)

-- A channel menu.
for _, channel in NetworkService:GetChannelsAsync() do
    print(channel.Channel, channel.Players, channel.MaxPlayers)
end
NetworkService:SwitchChannel(player, 3)

-- Join a friend, wherever they are.
NetworkService:TravelToPlayer(player, friend.PublicKey)

NetworkService.ChannelMerging:Connect(function(seconds: number)
    -- show "this channel closes in {seconds} s; you will move to another"
end)
```

## Proof

- A town with `max_players = 50` and 200 bots: channels 2 to 4 open as they
  fill, each under its limit; the bots leave and the channels close; a quarter
  left in channel 3 is merged into channel 1 with the warning first.
- A party of five travelling together lands in one channel; a friend's
  `TravelToPlayer` lands in theirs.

## As built

- A channel is a world of the manager's own with the `[[world]]`'s settings,
  named `town:2` and on, and a channel number; channel 1 is the world itself.
  Its saves are its world's folder (§4); its log, `logs/town-2/`, since no
  Windows file name may have a colon. The engine reads its channel from its
  `--managed` name before its first script runs.
- **Who is where**: the manager counts each channel's players as their
  arrivals and departures say -- not a heartbeat a second old -- and who is on
  the way there by key, until they arrive or their pass lapses, so a door
  that sends forty joiners in a second does not send them all to the one
  that had room for one (ADR 0232's As built found the heartbeat's count
  letting four into a town of two).
- A merge is a drain and a set of passes (what `update` does to a world, ADR
  0220 §7) to the fullest channel with room for every player of it; with
  none, it stays open and is looked at again after its while.
- **Saves shared by channels** (§4): a slot is written by one process at a
  time, under a lock on `<slot>.lock` held for its two renames and waited
  for up to two seconds; each save is whole (written aside and renamed in),
  and **the last save to finish is the slot** -- never merged. What several
  channels write belongs in one channel's slots (or a player's own slot),
  and the rest hear it through `CrossWorldService`. The write was already
  whole without the lock; what the lock adds is that the backup is always
  the save before the current one (`save_tests`: two writers, 150 saves
  each, of one slot).
- **A player who left is not sent** (`Travel`, `TravelGroup`,
  `SwitchChannel`, `TravelToPlayer`): no error; `TravelGroup` returns who
  went, the others whether they did. A party member who leaves while
  `CreateInstanceAsync` waits raised on the travel after it -- a trap the
  47-realms town fell into under 200 bots, and every game would.
- **Found on the way**: a world new to the game -- a channel, an instance --
  had no folder for its log, and wrote none (`Could not open the log file`).
  The manager makes it before it starts the world. And the 47-realms town sent
  a party, and told players, after some of them had left while a dungeon was
  made: `Realm.here` asks first.

## Measured

`examples/47-realms` with the town at `max_players = 50` and `channels =
true`, 200 light bots (ADR 0236) through the front door, development build:

- Channels 2, 3 and 4 opened as each filled; the manager's `worlds` then read
  50, 49, 47 and 44 players, none over its limit, every bot in.
- The bots gone, channels 2 to 4 closed after their twenty seconds empty.
- The merge -- warned, then everybody moved together to the fullest with
  room -- the party kept in one channel, `TravelToPlayer` to a friend's
  channel, `SwitchChannel` and the list are `engine_app_tests` (a manager of
  scripted worlds, and a channel's own script).

## Consequences

- One `servers.toml` line makes a town take any crowd the machine can run.
- The machine's own limit stays: channels are processes, and the manager's
  memory check (ADR 0221) refuses one it cannot hold.

## Not decided here

Channels on several machines (as worlds, a later record).
