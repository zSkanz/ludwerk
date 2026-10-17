# 0231 — A player sees the world their progress has

- Status: accepted; built
- Date: 2026-10-17
- Builds on: ADR 0076 (interest), ADR 0229 (priority), ADR 0230 (channels)

## The question

A bridge is broken until a player repairs it in a quest; after that, for
them, it stands -- while for the player beside them, who has not, it is still
broken. A dragon is dead for the party that killed it. This is phasing: the
same place, different state per player, in one world. Interest decides how
far a player can see; nothing decides *which version* of a thing they see.

## Decision

### 1. A phase is a tag on an instance

`Instance.Phase` is a string, empty by default. A thing with no phase is seen
by everybody, as today. A thing with a phase is seen only by the players who
are in that phase.

### 2. A player is in a set of phases

`Player:AddPhase(name)`, `RemovePhase`, `HasPhase`, `GetPhases`. A player's
phases are the server's to set and are sent to that player's machine, which
reads them with the same calls. Nothing else of a phase crosses the network:
a thing whose phase a player lacks is not sent to them at all -- not
hidden, absent -- and is removed from their machine when they leave the
phase, as a thing out of range is.

### 3. On top of interest, not instead of it

A thing must be in range *and* in phase. The check is the per-peer interest
pass (ADR 0228) with one more comparison; a phased thing costs a player who
cannot see it nothing.

### 4. A phased thing collides only for those who can see it

The server simulates every phase at once, and a client never holds a thing
outside its phases -- so its prediction cannot collide with one. If the
server let a player's character collide with a wall their machine does not
have, every step there would be a correction. So by default:

- a phased part **never** collides with the characters, and the parts they
  own, of players who are not in its phase;
- it **always** collides, as any part does, for those who are.

The physics filter carries each body's phase as a bit in a mask, and each
player's character and owned parts carry the mask of the player's phases: at
most **64 phases** in one world, and `AddPhase` past that raises. Everything
else -- NPCs, loose parts, another player's things -- collides with a
phased part as it would with any part.

A game with its own rules opts out per part with `PhaseCollision = false`,
and then decides itself, with `CollisionGroup`, who stands on what.

`Player:CanSee(instance)` answers the same question for a game's own hit and
interaction code.

## Script surface

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

-- The bridge, both ways, in the same place.
brokenBridge.Phase = "bridge-broken"
fixedBridge.Phase = "bridge-fixed"

local function bridgeFor(player: Player, repaired: boolean)
    player:RemovePhase(if repaired then "bridge-broken" else "bridge-fixed")
    player:AddPhase(if repaired then "bridge-fixed" else "bridge-broken")
end

NetworkService.PlayerAdded:Connect(function(player: Player)
    bridgeFor(player, saveOf(player).bridgeRepaired)
end)

-- A game's own hit code asks.
if attacker:CanSee(target) then
    -- the hit counts
end

-- A game that wants another rule for one part decides it itself.
oddWall.PhaseCollision = false
```

## Proof

- Two players at the bridge, one repaired it: each machine holds only its own
  bridge; the other's is not in its tree, nor in what it was sent.
- Leaving a phase removes the thing on that machine within one snapshot.
- A player out of the wall's phase walks through it with zero corrections;
  a player in it is stopped by it, also with zero corrections.
- The 65th phase in one world raises, saying the limit.

## As built

- `Instance.Phase` lives in a pool of its own, held by the few instances
  that have one; a world with none pays nothing -- not in the interest pass,
  not in the physics step. An instance's effective phases are its own and
  every one above it: the interest pass decides an entry hidden when its own
  phase, or its parent's verdict, says so, in the same pre-order walk.
- A player's phases go to their machine as a message of their own, whole,
  when they change (`Phases`, protocol 55), a seat at a time: a machine's
  guests have phases of their own.
- **The physics filter** is the backend's pair hook: each body's phase as
  its bit plus one, and what a player's character and owned parts carry -- a
  mask of the bits of the phases the player is in. A body pair is refused
  when one is phased and the other carries a mask without its bit; a
  character's contact with a phased body, the same. Neither kind: a pair as
  any other. Jolt has it in its contact validation and its characters'
  contact listener; Box2D in its custom pair filter (`b2World_SetCustomFilterCallback`),
  for a part on the plane and the `Character2D` of a player; the null
  backend collides nothing to begin with. On the plane, a player's
  `Character2D` carries their mask and their other parts do not yet: a
  `Part2D` has no owner field for the sync to read it from.
- `PhaseCollision` is `BasePart`'s, as §4 has it; `Instance.Phase` and
  `PhaseCollision` at their defaults are in no world's hash.

## Measured

- `engine_replication_tests`: two bridges in two phases at one place; the
  player in one has that bridge and not the other, nor the plank on the other;
  moved to the other phase, the one comes with its plank and the other leaves
  within two snapshots; their machine reads its own phases; the 65th phase a
  world names is refused.
- `engine_app_tests`, over a link of three frames each way with real physics
  on both machines: a player out of a wall's phase walked through it (19.9 m)
  with no correction, and one in it was stopped at it (4.48 m from where it
  started, at the wall's face) with no correction either.
- `engine_physics_tests`, Jolt: a phased wall stops a character carrying its
  bit, not one carrying another's, and stops one carrying none; a phased
  floor holds a player's crate carrying its bit and lets one outside fall
  through. Box2D: the same wall, the same three bodies, the same answers.

## Consequences

- A world can tell a story per player without a copy of itself per player.
- A phased thing is server-authoritative like everything else; there is
  nothing a client can ask to be shown.

## Not decided here

Phased terrain (the ground is shared by everybody).
