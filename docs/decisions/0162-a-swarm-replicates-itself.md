# 0162 — A swarm replicates itself: agents as rows on every machine, sent when they would be wrong

- Status: accepted
- Date: 2026-10-04
- Decided by: the owner, who approved native swarm replication through
  ludwerk-08 (2026-10-04); the design is the agent's, under the standing rule
  to decide as professional engines do and record it
- Amends: ADR 0156, whose consequences said "agents are not replicated as
  agents", and its amendment of 2026-10-03, which left a match's horde to be
  mirrored by the game

## Context

A horde in a match is simulated by the host and drawn by everyone. ADR 0156
left the drawing to the game: it packs the horde in Luau fifteen times a
second, eleven bytes an enemy, in three tiers by distance, and each friend's
machine unpacks it and moves bodies of its own. Measured on the horde test
game with about 950 agents: 100 KB a second to each friend, on the reliable
channel because a script had no other (ADR 0161 has since given it one), and
Luau time on both ends that grows with the horde.

The engine has what the script does not: every agent's position each tick
without a call, each replica's clock, and the wire.

## How mature engines do it

- **Unreal's Mass** replicates entities through bubbles: per client, the
  entities relevant to it, with level of detail by distance deciding how often
  each is sent, and the client's Mass representation drawing them. Agents are
  not actors.
- **Unity's Netcode for Entities** sends ghosts as snapshots: delta-compressed
  against what the client acknowledged, quantised, prioritised by importance
  and distance, with interpolated and predicted ghosts on the client.
- The older and simpler rule both grew from is **dead reckoning** (DIS, then
  every vehicle and crowd game since): send a position and a velocity, let the
  receiver carry it forward, and send again only when the receiver's guess
  would be wrong by more than a threshold.

They agree that a crowd is rows on both ends, that relevance is per client and
by distance, that precision is quantised, and that what is sent is decided by
what the client would otherwise get wrong.

## Decision

### What a game writes

1. **`Swarm.Replicates`** (boolean, default false). On, the swarm's agents
   reach every replica as agents. Off, nothing changes: a swarm is where it
   runs, as in ADR 0156. The `Swarm` instance itself now replicates either
   way, so a replica has something to ask.
2. **A replica has the same swarm, read-only, with the same agent numbers.**
   `GetAgents`, `GetPositions`, `GetAgentPosition` and `QueryRadius` answer
   from what it has been sent, carried forward to now. What changes a swarm --
   `AddAgent`, `AddAgentAt`, `RemoveAgent`, `SetAgentSpeed`,
   `SetAgentPosition`, `Push`, `SetTargets`, `SetAgentTag`, the obstacles --
   is the authority's, and refused on a replica with a keyed error. Its
   properties are the authority's too: what a replica writes to one changes
   nothing there.

   **An agent's number is the same on every machine for its whole life.** A
   game's own messages name agents by it: a boss's health bar, a hit flash
   aimed at one enemy. A number is used again only after its agent was
   removed, and a replica is told of the removal before it is told of the
   agent that takes the number: both are reliable, in order, and ahead of the
   game's own reliable messages of the same tick. So on any machine a number
   names one agent from its `AgentAdded` to its `AgentRemoved`, and a message
   that names it in between means that agent.
3. **A tag an agent carries**: `Swarm:SetAgentTag(agent, tag)` on the
   authority and `Swarm:GetAgentTag(agent)` anywhere. A whole number from 0
   to 65535 that means what the game says: a kind in some bits, flags in the
   others. It is what a replica picks a mesh and a pose from.
4. **Three signals, on every machine**, deferred like every other:
   - `AgentAdded(agent, tag, size)` -- an agent this machine now has, and how
     large it is (its width, its height, its width);
   - `AgentRemoved(agent, tag, position, reason)` -- one it no longer has,
     with the tag and the place it had last, and why
     (`Enum.SwarmAgentRemoval`): `Removed`, the authority removed it, or
     `OutOfReach`, it walked out of what this machine is sent;
   - `AgentTagChanged(agent, tag)`.

   On the authority they follow `AddAgent`, `RemoveAgent` and `SetAgentTag`.
   On a replica they follow what arrives -- and an agent that walks out of a
   replica's reach is removed there, and added again when it comes back. One
   script draws the horde solo, hosting and joined.

   **The last tag travels with the removal**, reliably. A game that sets a
   "killed" bit and removes the agent in the same tick has that bit in every
   machine's `AgentRemoved`, with no race against the unreliable positions:
   a death is played where the agent was killed, and nothing where it only
   walked away.
5. **The game draws; the engine places.** `Swarm:SetAgentBody(agent, body)`
   gives an agent a `BasePart` of this machine's own to move: each frame the
   engine writes its `CFrame` -- where the agent is drawn, facing the way it
   walks -- and nothing else about it. `nil` lets it go. On the authority it
   is the body `AddAgent` took, changed. A replica's bodies are its own
   instances and go nowhere.
6. **Whose eyes: `Player.ReplicationFocus`** (a `BasePart`, or nil). What a
   player's machine is sent is decided round a point: the player's character,
   or this part when it is set. It is the engine's one notion of where a
   player is looking from -- for a swarm's agents and for the parts streamed
   to that player alike. A fallen player watching a friend is the friend's
   character in this property.

### What the engine sends

7. **Reach, per replica**: the agents within `Swarm.ReplicationRadius`
   (default 80 m) of that player's focus, in at the radius and out at a
   quarter more, as the parts streamed to it are.
8. **Coming and going are reliable; where they are is not.**
   - Agents entering and leaving a replica's reach go on the Control channel:
     the number, the tag, the size, and where it stands. A replica never
     keeps a ghost of an agent that died, and never misses one that was born.
   - Positions go on a channel of their own -- `Swarm`, number 5, unreliable
     and not sequenced -- in messages of at most 1100 bytes, each standing
     alone: a lost one is some of the horde, late. Not sequenced because each
     agent is ordered by the tick of the message that names it: on a link
     whose packets swap places, a sequenced channel throws away the one that
     arrives second, and measured at twenty milliseconds of jitter that was a
     third of them.
9. **An agent is sent when the replica would be wrong about it.** For each
   replica the authority keeps what it last sent of each agent -- a position,
   a velocity, a tick -- and each tick works out where that replica is drawing
   it now. It sends the agent again when that is off by more than a threshold
   that grows with distance (a fifth of a metre near, a metre and a half at
   the edge of reach), when its facing turned, when its tag changed, or when the message
   that last carried it was lost.

   **A replica says which messages arrived** (`SwarmAck`, a few bytes a tick,
   each said several times). A message of positions nobody acknowledges within
   its round trip and half again was lost, and its agents are told again at
   once. Without it the authority takes what it sent for what the replica
   has, and a lost message leaves an agent wrong until its turn comes round:
   measured over a link losing a third, agents drawn ten metres from where
   they were. A refresh every two to four seconds is kept for whatever that
   misses.
10. **Quantised**: a position to an eighth of a metre, twelve bits an axis
    from an origin the message carries (a 16 m lattice near the focus); the
    heading in eight bits and the speed in six. **The height is how far above
    the ground the agent is** -- the ground a replica finds in its own copy
    of the world -- in eight bits of an eighth of a metre, and left out when
    that is nothing: an agent walking is five bytes, and a flier, or one
    standing on the pile, is six. Past thirty-two metres up it is sent whole.
    The agent's number is a small step from the one before it.
11. **A budget a replica**: at most 29 KB a second of positions to one
    replica. Past it the agents wait their turn, the most wrong first.
12. **Tick stamped.** A position message says which tick it was true at. A
    replica ignores one older than what it has for that agent, and one older
    than the agent's own arrival -- a number is reused when an agent dies, and
    a late message for the one that had it before must not move the new one.

### What a replica draws

13. **Carried forward, and eased.** A replica draws an agent where its last
    position and velocity put it at the authority's tick, as the newest
    message has it -- for at most five seconds, past the longest an agent goes
    untold, then it stands. When a newer position
    arrives, the difference from where it was drawn is taken up over a tenth
    of a second rather than at once. So an agent moves every frame at any
    update rate, and a correction is a lean, not a step.
14. **Placed on the tick, drawn between ticks.** A replica's swarm is not
    stepped: each tick its agents are carried forward and their bodies' `CFrame`
    written, as the authority's are, and a body is then drawn between ticks
    like any part that moves (ADR 0134).
15. **The ground is not looked for with a ray.** An agent's height travels as
    how far it is above its ground: the terrain's top under it where a terrain
    covers the place -- which a replica reads from its own copy -- and where
    none does, a floor the message carries. What a ray finds on a replica is
    that machine's world: its own bodies, and none of what did not replicate.

### The wire

16. Protocol 38, with ADR 0161: channel 5 (`Swarm`); messages `SwarmAgents`
    (27, Control), `SwarmState` (28, Swarm) and `SwarmAck` (29, Swarm, from
    the replica); `Swarm` among the replicated
    classes, with `Replicates` and `ReplicationRadius`; `Player` gains
    `ReplicationFocus`.

## Measured

A dedicated server and three friends as separate processes, each friend's
link 150 ms long with 20 ms of jitter and 2% of its packets lost (the engine's
network simulator); 1500 agents after three players walking in circles, thirty
killed and thirty born a second, twenty tags changed a second:

| What | Target | Measured |
|---|---|---|
| Positions to one friend | | 28.4 KB a second |
| Everything to one friend | under 40 KB a second | 38 KB a second |
| The furthest an agent moves in a tick on a friend's machine | no visible stepping | under 0.5 m; a step over 0.4 m in 1 to 6 of 450 000 agent-ticks, none over a metre |
| A friend who joins has every agent | within a second | 0.35 to 0.4 s |
| The host's cost | under 0.3 ms a tick | 0.22 to 0.37 ms a send, every second tick: about 0.15 ms a tick |

What the first measurements were, and what each cost:

- **A refresh every third of a second near** was 31 KB a second on its own.
  Acknowledgements replaced it.
- **The instant walk of an agent in a packed crowd** -- blocked one tick,
  shoved the next -- had every agent wanting to be told twenty times a second.
  Smoothed over a few steps, half as many.
- **A replica that found the ground with a ray** stood agents on whatever its
  own world had there: two metres up, on a player's head, since `CanQuery`
  does not replicate. It reads the terrain it has, or the floor a message
  carries.
- **Carried forward for half a second only**, an agent walking a straight
  line was stopped and corrected every forty ticks, a metre and three
  quarters each time.
- **A clock pulled towards the newest message every tick** stood still when
  no message came -- which is exactly when an agent is being carried: eleven
  metres behind.
- **A sequenced channel** lost a third of the messages to twenty milliseconds
  of jitter; some agents went a second untold and jumped four metres.
- **Positions for a number used again** reached the agent that had it before
  when the reliable message was sent a second time. An agent's position is
  sent only once its coming was acknowledged.

## Trade-offs, stated

- **A replica's agent is a guess between messages.** It is where the last
  velocity carries it. A weapon's hit is the authority's question
  (`QueryRadius` there), never a replica's.
- **Not the stack's every wobble.** An agent pushed about inside a packed
  crowd by less than the threshold is not sent again until its refresh. The
  crowd reads as a crowd; no one agent is exact.
- **A replica has only what is in its reach.** `GetAgents` there is the
  agents near its player, not the horde.
- **The tag is the whole of what the engine carries for the game.** Health,
  a target, a timer: the game's own, sent its own way if a replica needs it.

## Consequences

- The game's snapshot code goes: it sets a tag, and draws what it is told
  about.
- A swarm's agents on the authority need no bodies (`AddAgentAt`) when the
  authority's own view draws them the way a replica's does.
- Tests:
  - `session_tests.cpp`: over a link that loses a third and reorders a
    seventh, two hundred agents all arrive, with their tags, drawn near the
    truth; a removal carries the last tag and its number names the next agent
    in that order; a replica has what is round its player's focus, and the
    focus can be named; a swarm that does not replicate sends no agents;
    fifteen hundred arrive in three ticks and their positions stay under the
    budget; an agent walking a straight line is told a handful of times in
    seven seconds and stays within 0.8 m.
  - `swarm_tests.cpp`: what a swarm keeps of its agents' comings, goings and
    tags; a replica's row carried on a clock of its own, a correction taken
    up rather than jumped to, an older word refused; the floor, the lift and
    the body.
  - `world_host_tests.cpp`, from a script: the three signals, the tag, a body
    given and let go, and a replica's copy refusing what changes it.
