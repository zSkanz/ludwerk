# 0163 — Another player's character is met where the authority will have it, not where it is drawn

- Status: accepted
- Date: 2026-10-04
- Decided by: ludwerk-08, coordinating for the owner (2026-10-04: "other
  players' characters extrapolated to the present for the local character's
  collision, for games that want heroes to block each other"); the design is
  the agent's, under the standing rule to decide as professional engines do
  and record it
- Builds on: ADR 0076 (prediction and interpolation), ADR 0133 (the predicted
  island), the multiplayer smoothness brief (replicated parts are passable for
  the local character)

## Context

A replica draws every character but its own a few ticks in the past, between
two snapshots (ADR 0076), and steps its own ahead of the authority by as many
ticks as it has intents unanswered. Its own character collided with the
others where they were drawn.

Two players running side by side are, for the authority, side by side. For
the one behind, the other is drawn a metre and a half further back than it
is -- on top of him. His prediction is stopped by a player who is not there,
the authority's is not, and he is corrected at every snapshot for as long as
they run together. D534's measurement, on the horde test game with one hero
following another for forty seconds: 137 corrections of his position, 0.17 m
at the median.

A game whose players need not block each other puts them in a collision group
that does not meet itself (D545 made that reach replicas) and never pays
this. A game whose players must block each other -- a shooter, a brawler --
could not opt out.

## How mature engines do it

- **Unreal**: simulated proxies are interpolated, and the autonomous proxy's
  movement collides with their capsules where they are drawn. The rubber band
  of walking into another player is a known cost, and many games turn
  pawn-against-pawn blocking off for it.
- **Source**: players are predicted against interpolated players, with the
  same cost; team-mates commonly do not collide.
- **Rocket League, and games built on full prediction**: every other car is
  simulated forward on the client to the present from its last known state,
  and corrected when the server says otherwise. The collision is right far
  more often than not, at the price of a guess that can be wrong.
- **Overwatch**: other players push softly rather than block.

The choice between them is what a wrong guess costs against what a stale
position costs. A character's walk over a quarter of a second is close to a
line; the stale position is wrong by the whole distance every time two
players move together.

## Decision

**On a replica, another player's character is collided with where the
authority is expected to have it when it applies the tick this machine is
predicting.** No property, no call: a game that lets its characters collide
gets it, and a game that does not is unchanged.

- **Where that is.** The authority will apply the intent being predicted now
  as many ticks after the newest snapshot as this machine has intents
  unanswered. So the other character is expected at its newest snapshot's
  place, carried on by the way it went between the last two snapshots, for
  that many ticks -- at most fifteen, a quarter of a second, past which a
  guess is worse than the last known place. Across the ground only: a jump
  carried forward is a head in a ceiling.
- **What is at that place.** A stand-in: a copy of the character's capsule,
  put there every tick (`IPhysicsBackend::setCharacterStandIn`). Characters
  collide with it, by the collision group the character is in; bodies do not
  -- the character itself is still in the world, where it is drawn, and a
  crate must not be pushed by both.
- **What is at the place it is drawn.** The character, as before, for
  everything but characters: a raycast and an overlap see it where the player
  sees it, so a shot aimed at what is on the screen hits what is on the
  screen. Its body no longer stands in a character's way there, and two
  characters touch (`Touched`) where they meet -- at the stand-in.
- **Who decides.** The session, each time it draws the others
  (`CharacterBodyComponent::collisionLead`): the expected place less the
  drawn one. The physics mirror keeps the stand-in that far from the
  character; an authority, and a replica's own character, have none.

## Measured

The horde test game, its heroes made to collide with each other, one hero
following another for forty seconds:

| | corrections of position |
|---|---|
| before, 1 ms each way | 137, 0.17 m at the median |
| after, 1 ms | 9 |
| after, 75 ms each way | 7 |
| after, 150 ms each way | 10 |

Those left are one to six centimetres: a guess off by a little as the one
ahead turns.

## Trade-offs, stated

- **A guess can be wrong.** A player who stops dead is, for a quarter of a
  second at the worst, expected up to two and a half metres past where he
  stopped at a run; a character walking into that stand-in is stopped a step
  early and corrected forward. It was wrong by more, every tick, before.
- **Two characters pushing into each other are still corrected**: what each
  will do next is the other's input, which no machine has yet.
- **Drawn and met in two places.** A character can be seen to stop a little
  short of another, or to brush past one it appears to touch. The picture is
  of the past; the collision is of the present.
- **The stand-in is not replayed.** When the replica's own character is
  stepped again after a correction, the others stand where they are expected
  now, for every tick stepped again.

## Consequences

- `IPhysicsBackend::setCharacterStandIn`: a character's stand-in, made, moved
  and taken back; Jolt keeps a kinematic body that carries no record, so no
  query sees it, and refuses its contacts with bodies.
- Tests: `physics_tests.cpp` -- a character walks through one that has a
  stand-in elsewhere and is stopped at the stand-in; a ray does not see it;
  without it the character blocks again. `session_tests.cpp` -- a replica
  expects a running character at or past its newest snapshot and a standing
  one where it is drawn, and carries no jump forward. `physics_sync_tests.cpp`
  -- a replica's mirror keeps the stand-in where the session says and takes it
  back; an authority's has none.

## Not decided here

- **Other players' loose parts.** They stay passable for the local character
  (the smoothness brief); the island it pushes is predicted (ADR 0133).
- **Soft collision**, where players push each other apart gently instead of
  blocking: a game's rule, written in its predicted step.
