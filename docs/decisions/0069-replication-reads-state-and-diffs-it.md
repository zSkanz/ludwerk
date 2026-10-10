# 0069 — Replication reads state and diffs it, against a declared wire schema

- Status: accepted
- Date: 2026-08-27
- Milestone: N1 (post-v1 phase 4), part A
- Decided by: the agent, under the owner's standing instruction of 2026-08-26 to
  take the repository's decisions on their behalf. The **milestone** was the
  owner's own: *"terrain editor multiplayer voxels etc."*, 2026-08-27.
- Depends on: [ADR 0070](0070-a-port-is-opened-by-a-posture-and-never-by-a-script.md)

## Context

The engine is deterministic on a fixed tick, has a world hash, has an
`ITransport` seam with an ENet implementation, and has two `WorldHost`s able to
run side by side in one process. What it has never had is a reason for a second
world to agree with the first.

**The obvious delta source is `scene::ChangeQueue`, and it cannot work.** This
is the one paragraph worth putting in an ADR rather than a comment, because it
is the design somebody proposes every six months and the refutation is two
citations long:

- `PhysicsSync` writes transforms **straight into the component**, under a
  comment that names itself *the QUIET write*
  (`engine/scene/src/physics_sync.cpp:963-966`). It does not go through
  `setProperty` and it does not enqueue.
- `World::setProperty` enqueues **only when something is subscribed**
  (`engine/scene/src/world.cpp:626-634`).

So the most-replicated fact in any game — where things are — never reaches the
queue at all, and the second most-replicated facts reach it only by accident of
who happens to be listening. A replication layer built on the change queue would
work in a test scene and lose every moving object in a real one.

The second thing worth deciding before any code: **what a wire format is allowed
to be.** A hand-rolled serialiser drifts from the state it serialises, silently,
in the direction that matters — a field added to the world and forgotten on the
wire is a replica that is confidently wrong about the state deciding the next
tick.

## Decision

**1. Replication reads state and diffs it. It does not listen.**

Each send, the authority walks the instances in a peer's interest set, extracts
the fields the wire schema declares, and compares them with the last baseline
acknowledged by that peer. What differs is what is sent. This costs a walk the
change queue would have avoided, and it is the only design that cannot silently
miss a write — which is the property that matters, because the failure mode of
missing one is a replica that looks right and is not.

**2. The wire schema is declared, generated and checked — like the API is.**

`api/wire/*.wire.luau` declares every replicated field beside the class it
belongs to; `api/generator/gen_wire.luau` generates the C++; and
`tools/repo/wirecheck.luau` refuses a tree where the generated form and the
declaration disagree. This is the shape `api/defs/` already has, for the same
reason it has it: two hand-maintained lists of the same thing is one list plus a
bug waiting for someone to edit only one of them.

**Field ids are permanent.** A removed id goes to a `Retired` list with the
version it died in, and reuse is refused by the checker. An id that means one
thing to a server and another to a replica is not a bug anybody can debug from
the symptom.

**3. The correspondence gate, which is the part that is easy to skip.**

The wire schema's non-property entries and `world_hash.cpp`'s hand-written block
are two hand-maintained lists of the same simulation state, and a field in one
and not the other diverges a replica *in exactly the state that decides the next
tick*. A test compares the two lists by name and fails on a difference in either
direction. It is a list-versus-list assertion, which is cheap and is the only
thing that catches the class.

**4. The four postures are `NetworkService`'s to report and never to set**, per
ADR 0070. `Authority`, `Topology` and `ServerTick` are read-only and `HostFact`,
so they are out of the world hash — a fact about the machine, not the world.
Their defaults **are** the solo truth: authority true, topology solo, one
player. A script writes `if NetworkService.Authority then` and that branch is
present *and taken* when nothing is networked, which makes it a gameplay branch
("do I decide this") rather than a configuration branch ("am I networked").

**5. A replicated instance in a replica world is `driven`.**

Two lines at `physics_sync.cpp:212`. Its body becomes Kinematic, and `writeBack`
skips kinematic bodies (`:948-958`), so the solver neither fights the incoming
deltas nor overwrites them — with no branch in game script and no second
transform authority. This is the same mechanism a welded part already uses;
nothing new is invented for it.

**6. Losing interest is `InstanceStreamedOut`, verbatim.**

An instance a replica stops caring about is reparented to nil and reported
through the streaming husk contract (`streaming_glue.h:9-14,49-57`). A replica
losing interest and a chunk being evicted are the same event from a script's
point of view, and the API already has the word for it. Inventing a second one
would mean every game handles two spellings of one thing.

**7. `Terrain` is not replicated, and that is a decision rather than an
omission.** Its bulk is not a property, a field is megabytes, and a sculpt is
rare and authored. A world's terrain arrives with the world — from the scene, or
later from a streamed cell — and the wire schema records the exclusion by name so
that the next person to look does not have to work out whether it was considered.

**8. The replica's hash is not the server's and must not be compared to it.**
Interest management gives the replica a strict subset of the world, so the two
hashes differ by construction. The per-baseline checksum catches *apply* bugs,
which is what it can honestly catch. It cannot detect simulation divergence, and
a design that claimed otherwise would be claiming a guarantee it does not have.

## Consequences

**The acceptance test already exists and only has to be inverted.**
`runTwoWorldsGate` runs two `WorldHost`s and two Luau VMs in one process with a
pixel differential proving the two worlds render *different* images
(`engine/app/include/luaug/app/two_worlds.h:66`, whose own header names
networking as the second caller). Driving world B as world A's replica over a
loopback transport makes the acceptance test that they render the **same** one.
Headless, deterministic, no network involved, and the harness is built.

**Every determinism trace moves at tick zero, once.** `NetworkService` is
`Service`-tagged, so `registerServices` instantiates it at boot
(`services.cpp:1097-1133`), plus one `Player` in solo. `HostFact` on the three
properties is what stops it recurring on every later change to them.

**Two defects in code we already own have to be fixed before the design can
trust its own transport**, and both were found by reading rather than by
failing:

- `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT` is never set in `flagsFor`
  (`engine/net/src/enet_transport.cpp:40-54`), so any payload over roughly one
  MTU sent as `UnreliableSequenced` takes the reliable-fragmented branch
  (`third_party/enet/peer.c:135-145`). That is head-of-line blocking, which is
  precisely what the mode exists to avoid, and no test catches it because the
  loopback case sends four bytes. It needs a 4 KB test.
- `ITransport` has never had a production caller, so N1 owes a deterministic
  loss-and-reorder decorator over its six virtuals, seeded from `core::Pcg32`
  and never from a clock. The transport tests say so themselves
  (`transport_tests.cpp:6-10`): a bug that only appears under loss or reordering
  is invisible today.

## Deliberately not in this decision

Rollback and prediction reconciliation (ADR 0025 records determinism level B
rather than enforcing it, and `CROSS_PLATFORM_DETERMINISTIC` is OFF), a public
protocol commitment, `RemoteEvent`, `NetworkOwnership` and `Team`. Channel 3 is
reserved and named in the wire schema so that the numbering cannot shift when
one of them arrives, and nothing more is promised.

## Alternatives considered

**Diff the change queue.** Refuted above, with citations, because it is the
proposal that keeps coming back.

**Replicate the world hash and resimulate.** Rejected: it requires
cross-platform bit-identical simulation, which ADR 0025 explicitly does not buy,
and a replica that must resimulate cannot have a subset of the world — which
kills interest management, which is the only thing that makes a large world
affordable.

**A hand-written serialiser per class.** Rejected for the reason `api/defs/`
exists: it is a second list of the same facts, and the two drift in the
direction nobody notices.

## Amended, 2026-10-02 (protocol 33, the netcode audit)

- **A snapshot over 16 KiB goes reliable, in 32 KiB parts** (`SnapshotPart`),
  and nothing more goes to that peer until it is acknowledged; the state it
  was is kept until then, since the history may move past it. Before, a
  world's first snapshot -- every record whole -- was one unreliable message
  ENet fragments, lost whole when a fragment was and resent every other tick:
  29 seconds to join 1 000 parts on a loopback, never 5 000, and anything over
  a megabyte was refused by the transport and counted as sent.
- **An acknowledgement names a tick this peer was sent**, or it is refused.
- **The snapshot's name count is 32 bits**; sixteen wrapped past 65 535.
- **`Refused`**: an authority that will not take a replica -- full, or of
  another protocol -- says so before it lets the connection go.
- **The whole ground is encoded once** for every peer that needs it, not once
  a peer. Interest for the ground, and its own channel, wait for a design of
  their own: a chunk message ordered against the spawns it may name.

## Amended, 2026-10-03 (protocol 34, NA34)

- **A field may name another instance.** `InstanceRef` was `Parent`'s alone; a
  joint's ends and a weld's parts are references too. The authority reads them
  as its own instances and sends each as the peer's network id -- after the
  walk, since a joint may name what is captured after it -- and an instance
  that is not captured is none. A replica writes its own copy, and holds the
  field back until that copy has arrived, as it does a parent.
- **An exclusion holds below a replicated ancestor.** A `Bone` is an
  `Attachment`; the lookup that walks up a class's ancestors for its schema
  stops at an excluded name, which the generated header now lists.

## Amendment, 2026-10-06 (protocol 42): a send costs what changed

Measured on a listen host with one friend, in a game: `net.send` at a median
of 0.2 to 2.8 ms and a p95 of 8 to 10, at about two thousand draws. Measured
here, an authority with 2,700 replicated instances, one replica and sixty
instances moving a tick: **6.4 ms a send**, of a tick of 16.

A field is sixty-four bytes whatever it holds, so an entity is a kilobyte and
more and a state four megabytes. Every tick the authority read every field of
every instance into a new state, allocating four times an instance (2.3 ms);
copied every entity a peer is sent, compared every field of each with the
peer's baseline, and summed every byte of the copy for the checksum (3.4 ms a
peer); and encoded every instance's attributes into a tree and compared each
with the tick before's (0.8 ms), though one instance in thousands carries any.

- **An entity's bytes are hashed once**, when it is captured
  (`EntityState::hash`, `hashOf`), and three things read the number instead
  of the bytes: a peer's diff -- same number as its baseline's, nothing of it
  changed, no field compared; the checksum, now every entity's hash folded in
  id order; and the replica, which takes the number again only for the
  entities a snapshot made or changed. **That is a change to what the
  checksum is, so the protocol is 42**: both ends compute it, and a build
  that computes the other would fail every snapshot's.
- **A peer's world is a view of the whole**, not a copy.
- **A capture allocates nothing an instance**: ids in a sorted array, a child
  carrying its parent's id on the walk's stack, and field sets taken back
  from the state the history lets go each tick.
- **Attributes are read of the instances that carry any**
  (`World::carriesAttributesOrTags`): an instance known last send and not in
  the shadows carried nothing then.

After: **1.8 ms a send** in the same measure -- capture 1.7, the peer 0.2, the
attributes 0.01. What is left is the reading itself, every field of every
instance every tick; a world that says what changed would take that too, and
is not this amendment.

Held by counts, not by a clock (`Stats::entitiesCompared`,
`attributeBodiesEncoded`, `fieldSetsAllocated`; "an authority's send costs
what changed, not what there is"): in 120 ticks of that world, only what moved
is compared field by field, one attribute body is encoded a tick, and no field
set is allocated. `net.send` has three scopes under it: `net.capture`,
`net.changes`, `net.peers`.

## Amendment, 2026-10-06, the second: a capture reads what may have changed

What the first amendment left was the reading itself -- 1.7 ms of the 1.8,
and in a game the whole of `net.send`'s p95 (1.9 ms of 2.4, a host with one
friend).

- **An instance is kept from the capture before when the components its
  fields are read from are the same bytes** (`sourceDigestOf`,
  `EntityState::source`). The components' own bytes, so it does not matter
  who wrote them or how. Stamping a component when it is handed out to be
  written was tried first and says nothing here: the engine's own systems
  hand out every part and every body every tick, to read them. An instance
  is read again when its bytes differ, when it was renamed or moved in the
  tree, when it names another instance (whose leaving changes it and no byte
  of its own), when one of its components holds memory of its own (a
  character, a swarm), and **one tick in eight whatever the bytes say** --
  what a field is read from that is none of these is late by that much, and
  no more. One such thing is known and counted in: a material copy is the
  world's, in no component, and a part wearing one has the world's count of
  changes to those in its number (`World::materialClonesRevision`).
- **States share the field sets they have in common** (`SharedFields`). A
  kept instance is the very set the state before holds; a state takes a set
  of its own only to write to it. On the authority that is the copy a kept
  instance still cost. On a replica it is every snapshot: applying one copied
  the whole state before it, four megabytes for a world of 2,700 instances,
  and now copies the entities the snapshot changes. And a history of
  sixty-four states holds one set an unchanged instance, where it held
  sixty-four.
- **A class's schema is found once a world**, not asked of every instance
  every tick up its ancestors by name.

After: **under a millisecond a send** in the same measure with sixty moving a
tick (0.95; 6.4 before either amendment), and in a hosted match of two
processes 27 instances read a tick and 173 kept, of 200. The wire is what the
first amendment made it: protocol 42.

Held by counts (`Stats::entitiesRead`, `entitiesKept`): of 2,700 instances a
tick, the sixty that moved and an eighth of the rest are read, the others
kept, and no more than a field set a tick is allocated. And "every pool the
wire reads is one a capture can tell has not changed" fails when a pool is
added to the wire and not to the digest's list.

## Amendment, 2026-10-06, the third (protocol 43): an acknowledgement is not reliable

D576. A replica said it held a snapshot with a reliable message on the control
channel, one a snapshot. A reliable channel delivers in order, and the game's
own reliable messages -- every `RemoteEvent` and `RemoteFunction` a client
sends -- are on that channel: each acknowledgement a link lost held all of
them until it was sent again. Measured at 150 ms each way and 2% loss, 19 of
88 remote calls took a tenth to a third of a second longer than the link.

- **An `Ack` goes without a guarantee, on the state channel**, which carries
  nothing reliable from a replica and so is never held. The authority keeps
  the newest tick it heard. A lost one costs one diff against the state
  before, which the replica holds -- both ends keep sixty-four -- and once
  the authority has no state the replica proved it holds, it sends whole
  ones until it hears again. The guarantee had been for a replica that kept
  one state.
- **The acknowledgement of a snapshot sent in parts stays reliable**, on the
  control channel: nothing goes to that peer until it is heard, so nothing
  later could say it instead. An authority takes an `Ack` on either channel.

After: 5 of 88 late, each by one resend of the call itself -- the link's own
loss, and all of it.

**What is still in front of a client's remote calls** is the client's other
remote calls, in order, which is the guarantee a `RemoteEvent` gives; and
from the authority, spawns and attribute changes, which a message that names
an instance must not arrive before.

## Amendment, 2026-10-10 (protocol 44): what a character carries

A character a server script spawned -- by `Instance.stamp`, by `Clone` of a
template in `ReplicatedStorage`, or by `Instance.new` -- reached a joiner with
its attachments, decals and emitters and nothing else. Thirteen classes it
might carry were excluded, most for a reason that was true of something else:
a light "that moved because its parent moved needs no message", which is true
of the motion and says nothing of the light. A torch was dark on every
replica, a cape did not exist, a client's `WaitForChild("Footstep")` never
returned, and an emitter arrived without the sequences it is coloured by.

**The rule, the same for every class this adds**: the instance and what was
authored on it travel when it is made and when a property changes. Nothing is
sent a tick. What the class simulates -- a chain's swing, a trail's pieces, the
particles in the air, where a sound has got to -- stays each machine's own.

- **`PointLight`, `SpotLight`, `SpringBone`, `SpringCollider`, `Highlight`,
  `Beam`, `Trail`, `Sound`** join `Classes`, each with every authored
  property. **`Bone`** joins as an `Attachment` (`Extends`) with the joint it
  names and its `Transform`, which a script on the authority writes and is
  therefore state. **`ParticleEmitter`** gains what ADR 0160 gave it and the
  wire never learned -- its picture, frames, turn and collision -- and its
  three sequences; **`Decal`** its blend mode and its glow.
- **A sequence has an encoding**: `ColorSequence` and `NumberSequence`, a `u8`
  count and that many keys of four or three `f32` -- what an attribute of the
  type already is. It is the first encoding that says its own length, and the
  rule that every encoding is fixed-width is kept everywhere it mattered: a
  list, never a list of lists, bounded at the twenty keys a sequence may hold,
  and a count past that rejects the message. A list that decodes and is no
  sequence is held as sent -- both ends must hold the same bytes -- and not
  written to the instance.
- **A sequence is several cells.** A field's value is a sixty-four byte cell
  compared, hashed and kept by its bytes, and twenty colour keys are 321.
  Widening every cell for three classes would be paid by every field of every
  instance in every state; an interned name would never be freed, and a beam
  faded by a script each tick would grow both ends for as long as the match
  lasted. So a sequence's bytes lie over several cells -- the first in the
  field's own place, the rest after the instance's last field (`cellCount`,
  `furtherCells`) -- and the diff, the checksum, the baselines and the replica's
  record of what it wrote go on reading bytes. Three places know: a record is
  built from the fields and a sequence compared with all its cells; the writer
  and the reader lay it down as a count and its keys; the apply writes it whole
  when any cell moved. The published checksum says where the further cells are
  taken.
- **A `Sound` is played by each machine.** `Playing` travels as the property
  it is; a replica that sees it become true starts its own copy from the
  beginning, and one that sees it become false stops it. `TimePosition` is not
  on the wire. Accepted with it: a one-shot that starts and ends between two
  snapshots is not heard on a replica, a `Play` on a sound already playing is
  no change, and a late joiner hears a playing sound from its start. A game
  that needs every shot heard sends a remote. `Sound.Group`, `AudioGroup` and
  the nine `*SoundEffect` classes stay off the wire: a mixer is each
  machine's, and eleven classes of numbers for a change of tone are a client
  script's to make.
- **Still excluded, and why is said again where it was stale**: `BillboardGui`
  and `SurfaceGui` (the interface tree is each machine's own),
  `NavigationAgent`, `Ragdoll`, `Script`.
- **An exclusion may be `Quiet`** in the schema: the class is each machine's
  own by nature, or what it does reaches a replica some other way. Where a
  person is developing (`Config::developer`, from the dev run's flag), an
  authority says once a class that an instance of any OTHER excluded class,
  under an instance it sends, will be on no replica
  (`net.warn.class_not_replicated`). The capture already stops at such an
  instance and already asks each class's schema once, so the warning is one
  byte compared where the walk gives up.

Tests: `session_tests.cpp`, the five cases named "(protocol 44)";
`field_tests.cpp` and `extract_tests.cpp` for the encoding and the cells; and
`netcode_carried`, two processes, a thing spawned three ways and read back
class by class.

## Amendment, 2026-10-10, the second (still protocol 44): what a class already sent was sent without

The same audit, turned on the classes that were on the wire: every property a
script can write, against the fields its class has. The rule for which
travel: **a property travels when a replica needs it to draw the thing, to
predict it, or to answer a script's read or query as the authority would.**
What only the authority ever reads stays out, and says so.

- **A part's body** was sent `Anchored`, `CanCollide` and its group. A
  replica predicts the loose parts near its character itself (ADR 0133) from
  the body it holds, so `Friction`, `Restitution`, `Density`, the two
  dampings and `Buoyant` travel; and a client script's raycast, `Touched` and
  `Collided` are answered by that machine's bodies, so `CanQuery`, `CanTouch`
  and `ContactDetails` do.
- **A pivot**: `PivotOffset` on a part and on a model, and `Model.PrimaryPart`
  as a reference. `GetPivot` and `PivotTo` are answered where they are called.
- **`Lighting.OutdoorAmbient`**; a **`WaterPoint`**'s `Width`, `Depth` and
  `Sharp`; a **`Part2D`**'s `Density`, `Friction`, `Elasticity`,
  `FixedRotation`, `GravityScale` and `CollisionGroup`; a **`Tilemap2D`**'s
  `CollisionGroup`.
- **Withheld, by name, in the schema** (`Withheld` on a class, printed in the
  published protocol under it): `Model.StreamingMode`, `Water.BankWidth`, a
  `Swarm`'s nine steering properties, `Workspace.CurrentCamera`,
  `Sound.Group`. A joint's and a mover's properties are all sent, under the
  names of the fields they share.

Three things in the capture had to change for it:

- **An instance that names another is kept from the capture before**, as any
  other is. It never was -- what it names can leave with no byte of its own
  changing -- and that was affordable while only joints named anything. A
  model names its primary part, and a world's models would all have been read
  every tick. What each reference was numbered by the capture before is now
  in the number compared (`referenceAt`), so a part that left or arrived is a
  capture late in the model that names it, and then read.
- **A pivot's pool is known to the capture and not read by it.** Every part
  has a pivot and nearly none ever moves it; reading each every tick to learn
  that was a tenth again of a capture. A pivot moved on its own goes at the
  instance's next reading whatever its bytes say.
- **That next reading is counted in captures.** It was counted in ticks, one
  in eight, and a game sends every second tick: an instance whose number was
  of the other parity never had a turn. Nothing visible stood on it until a
  pivot did.

What it costs, measured where a send was measured before (2,400 parts in 300
models, sixty moving, one replica): a median send of 1.04 ms against 0.91.
Ten more cells a part are ten more to read, clear and hash each time a part
is read -- the sixty that moved and each instance's turn in eight -- and
nothing for a part that is kept. A whole record of a part is 76 bytes longer.
