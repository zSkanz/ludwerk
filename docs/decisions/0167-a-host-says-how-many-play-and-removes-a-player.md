# 0167 — A host says how many play, and removes a player

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on a finding from play (ludwerk-08, the horde test
  game), under the standing rule to decide as professional engines do and
  record it
- Builds on: ADR 0085 and ADR 0105 (hosting and joining from a script),
  NA8 (a full server says so), ADR 0100 (the published protocol)

## Context

A run of the horde test game takes four heroes. Up to thirty-two machines
could join it all the same, each sent every `FireAllClients`, because the
only limit was `--max-players` on the command line -- and a game a player
hosts from a menu has no command line. And a player in the room who never
readied held the start for everyone: nothing a script could do removed them.

## How mature engines do it

Every engine with a host has both, on the host: a number of players a
session takes, set by the game before or while it is open, with a refusal
the joiner is told; and a call that disconnects one player with a reason
their machine is given. A ban is the game's: a list it keeps and applies when
somebody arrives.

## Decision

1. **`NetworkService.MaxPlayers`**, a whole number from 1: the most players
   the match takes, **the machine's own among them** -- what `#GetPlayers()`
   may reach, which is the number a game thinks in. Written by the machine
   that hosts or will host, before `Host` or at any time after; a machine
   that joined reads 0 and may not write it. Somebody who joins a match that
   is full is refused as a full server always was (`Refused`, reason 2), and
   their `JoinFailed` says so.

   **Lowering it removes nobody.** A player already in stays until they leave
   or are removed. Never more than the match was opened for -- thirty-two
   others, or `--max-players` -- which is also what it reads while hosting
   when nothing was written.

2. **`Player:Kick(reason: string?)`**, the authority's to call on a player
   who joined. A client calling it is an error; so is the host removing its
   own player, who leaves with `Disconnect`. The player is told why and let
   go: their machine goes solo, its `Disconnected` fires with `reason` -- the
   game's own words, at most 512 bytes, or the engine's where there are none
   -- and it does not dial again by itself. On the host `PlayerRemoving`
   fires as for anybody who leaves.

   **Done at the frame's safe point**, not inside the call: a script that
   loops over `GetPlayers()` removing some is not having the list changed
   under it.

3. **It is not a ban.** Nothing stops a removed player joining again. A game
   that means one keeps the list -- by `UserId`, by an account of its own --
   and removes them when `PlayerAdded` says they arrived. The engine has no
   identity to ban by that a client could not change.

4. **The wire**: `Refused` gains reason 3, removed, and a `text` after the
   reason in every `Refused` (protocol 40) -- the game's words for a
   removal, empty otherwise.

## Consequences

- A lobby is a script: a size for the room, and a way out for the one who
  holds it up.
- The name is `Kick` because that is what every game calls it.
- Tests (`engine/app/tests/network_session_tests.cpp`): a host that sets two
  and hosts takes one friend and refuses the second with "full"; a client
  may neither kick nor set the limit; the host may not kick itself; a kicked
  friend hears the game's words in `Disconnected`, is offline, and the host
  has one player and heard `PlayerRemoving`.
