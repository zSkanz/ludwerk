# 0099 — Teams and network ownership

- Status: accepted
- Date: 2026-09-25
- Decided by: the agent, under the owner's mandate of 2026-09-24 ("do all of
  it", the depth the phases deferred) and their word of 2026-09-25 to build it
- Amends: ADR 0069 (which deferred `Team` and `NetworkOwnership` and reserved
  channel 3 for them) and ADR 0076 (where `Player.Character` was the one piece
  of ownership). Protocol 13 becomes 14.

## Context

ADR 0069 shipped replication without sides and without ownership, and kept
channel 3 so that neither would shift the numbering when it came. A game with
two sides has had to invent them in attributes, and a ball a player kicks has
moved a round trip after the kick: the authority simulates every loose body,
and a replica only predicts its own character.

## How mature engines do it

- **Unreal**: a team is game code (`PlayerState` carries it); ownership is an
  actor's owning connection, and physics replication lets the owner's
  simulation lead ("client-authoritative physics" is opt-in per body).
- **Unity (Netcode for GameObjects)**: `NetworkObject.ChangeOwnership`, and a
  `NetworkTransform` whose authority is the owner, sending its state up.
- **Godot**: `set_multiplayer_authority` per node; whoever is the authority
  sends the synchronizer's state.

All three end in the same place: sides are a small replicated record, and an
owned body is simulated by its owner and sent to everybody else, trusting the
owner within whatever checks the game adds.

## Decision

1. **`Team` is an instance, under `TeamService`.** `TeamService` is a service
   whose contents replicate to every replica, whatever the distance, as
   `ReplicatedStorage`'s do (ADR 0080). A `Team` has `Color` and `AutoAssign`,
   and `GetPlayers()`. Its name is its `Name`.
2. **`Player.Team` is the player's side**, set by the authority's game. A
   player who joins is put on the `AutoAssign` team with the fewest players, ties
   to the first in instance order; with none, they are on no team. It travels in
   the `Players` roster beside the character: user id, character, team -- three
   network ids per player, zero for none. A replica writing it changes its own
   copy and nothing else, as with every other replicated value.
3. **`BasePart:SetNetworkOwner(player?)` hands a loose part to a player**, and
   `nil` hands it back. Only the authority may call it, and only on a part that
   is not anchored; `GetNetworkOwner()` answers the player, or `nil` for the
   authority. The owner is a user id on the rigid body, so it means the same
   thing on both ends: the authority keeps every owner, a replica only its own.
4. **The owner simulates, and the authority follows.** On the owning replica
   the part is a dynamic body again -- no longer the kinematic follower every
   replicated part is -- and each tick its transform and velocities go up on
   channel 3, now named `Ownership` (`UnreliableSequenced`: a stale state is
   superseded by the next). On the authority an owned part is kinematic and
   moves to what the owner sent; everybody else sees it through the ordinary
   snapshot. Which parts a replica owns reaches it as a whole list on the
   Control channel, sent when it changes, as the roster is.
5. **The authority only takes what it gave.** A state for a part the sender
   does not own is dropped, and so is one for a part since handed back. An owner
   can still put its part anywhere; checking that is the game's, as it is in
   every engine above.
6. **A player who leaves gives everything back**, and a part that becomes
   anchored is the authority's again.

## Consequences

- Protocol 14: the roster grows a column, two messages are added
  (`Ownership` to a replica, `OwnedState` to the authority), and channel 3 has
  a name. A replica of 13 is refused at `Hello`, as every mismatch is.
- The owner's state is trusted. A game where that matters checks the state in
  its own scripts, or does not hand the part over.
- The owner's world hash is no longer the authority's for that part. Neither
  ever was for a replica (ADR 0069 decision 8); the authority's own trace stays
  deterministic, since what the owner sent is input to it.

## Alternatives considered

- **Team as a property of `Player` with no instance.** Rejected: a side has a
  colour and a list of members, and a script wants to find it by name.
- **Ownership by instance rather than by user id.** Rejected: a player instance
  does not exist on a replica for anybody but the players the roster names, and
  a user id is already what the roster and the intents speak.
- **The authority keeps simulating an owned part and blends toward the owner.**
  Rejected: two solvers over one body is the fight ADR 0076 ended for
  characters.

## Amended, 2026-10-02 (the netcode audit)

- **An owner moves what it owns within reach**: two metres a tick, 120 m/s,
  from where it is. Ownership was a licence to put a part anywhere in one
  message, the authority taking it back a tick later.
- **Joints are not yet on the wire** (NA34): an owner of a jointed assembly
  receives its parts only, and a vehicle falls apart on its owner's machine.
  The next slice of the netcode ledger.

## Amended, 2026-10-03 (NA34, D482)

- **Joints travel** (protocol 34): `Attachment`, every constraint, the welds,
  `NoCollisionConstraint` and the movers are on the wire, each with what its
  own properties hold. A replica solves a joint when it simulates one of its
  bodies -- owns it, predicts it (ADR 0133), or made it itself -- and leaves
  any other to the authority, whose snapshots say where the parts went. An
  owner's own change to a joint stays its own until the authority changes the
  same field. `Workspace.Gravity` travels too: an owner simulated under the
  gravity it booted with.
- **A part handed over starts from the authority's newest place** for it, not
  from where the owner last drew it: handed over in the tick it was made, it
  had been drawn nowhere, and the owner simulated it from the origin (D482).
- **An owner is followed at its reach, never refused.** A place further than
  two metres a tick is taken as far as two metres a tick go; a refusal was for
  good, since the owner never hears it and moves on from where it is.

## Amended, 2026-10-03 (protocol 35)

- **Teams withdrawn by the owner**: a side is the game's own state -- an
  attribute on the `Player`, which replicates (ADR 0106). `TeamService`, `Team`,
  `Player.Team` and `AutoAssign` are gone, and so is the roster's team column.
  Network ownership stands as written.
