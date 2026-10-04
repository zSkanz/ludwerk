# 0166 — The wire carries what changed: attributes and tags as edits, a rotation in eight bytes, input said once

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on a measured finding from play (ludwerk-08, the
  horde test game) and under the standing rule to decide as professional
  engines do and record it; the owner asked that tags be checked for the
  same defect, and they had it
- Builds on: ADR 0069 (replication), ADR 0106 (attributes replicate, whose
  "whole, not a diff" this amends), ADR 0100 (the published protocol),
  ADR 0076 and the multiplayer smoothness brief (intents with redundancy)
- Protocol: 40

## Context

A friend who joined a run of the horde test game received 41 KB a second.
With every one of the game's own remotes silenced on the host, it still
received 35. The game had spent the day cutting its traffic to 3.

`NetworkService:GetStats` said how many bytes and nothing about what they
were, so the cause was found by turning things off. Counted by the kind of
message they were in, on the friend's machine, in KB a second:

| | attributes | snapshots | swarm | the game's remotes |
|---|---|---|---|---|
| before | 24.0 | 9.1 | 5 to 8 | 0.7 |

**Attributes (D549).** ADR 0106 sends an owner's attributes whole whenever
any of them changes: "the replica makes the owner's attributes exactly these,
so a message received twice is simply the truth". It is simple, and it was
decided for a lobby's `Ready`. A hero carries eighteen attributes and one of
them is where it faces. Both heroes turned every tick, so both sets went,
names as text, thirty times a second. Tags ride the same message: one tag
put on sent every attribute, and one attribute changed sent every tag.

**Snapshots.** A moving body's record was 84 bytes, 62 of them its frame and
36 of those a rotation of nine floats -- for a character that never turns.
Protocol 1 left the rotation unquantised and said why: nobody had measured
the error against a physics mirror that reads the result back.

**Upstream.** A client standing still sent 6.9 KB a second: each tick's input
with the three before it, so that a lost message loses none, each of the four
written whole -- tick, action, type, three floats, a pressed byte.

Every one of these scales with the players in a match, and a relay bills by
the byte.

## How mature engines do it

- **Unreal**: properties replicate one by one, compared against a shadow, and
  only the changed ones are written; names cross as indices into a table the
  connection builds (`FName` export). Rotations are compressed -- a rotator in
  bytes or shorts a component, a quaternion as its smallest three. Movement
  input goes as saved moves, with the old ones combined when they are the
  same.
- **Unity Netcode / Mirror / FishNet**: dirty bits a field; transforms sent a
  component at a time with the rotation as a smallest-three quaternion in
  four to seven bytes, the position apart from it.
- **Source and its descendants**: delta-compressed entity fields against an
  acknowledged baseline, field by field; user commands sent with the ones
  before them for redundancy, each delta-encoded against the last.
- **Photon Fusion and Quantum**: bit-packed deltas, input sent redundantly
  and deduplicated.

All of them: what changed, and only that; a name once; a rotation packed; an
input that did not change is not repeated.

## Decision

1. **Attributes and tags travel as edits.** A new message, `AttributeEdits`
   (31, control channel, reliable and in order), carries for one owner the
   attributes whose value is another, the ones removed, the tags put on and
   the ones taken off -- and nothing else of the owner's. `Attributes` is
   kept for what it was first for: everything an owner has, once, to a
   replica that is sent the owner. A replica applies each edit in turn and
   holds exactly what the authority has, because the channel loses nothing
   and keeps order; "received twice is the truth" is no longer needed and is
   given up for it.

2. **A name crosses once a connection.** In an edit, an attribute's or a
   tag's name is a number: the first time, the number and its text; after,
   the number alone. The table is the connection's -- a replica that joins
   again starts from none -- and holds 32 767 names, past which a name is
   sent as text and not kept.

3. **A predicted character's attributes in a snapshot are named by hash**,
   four bytes where the text was. The replica's own predicted steps wrote the
   same attributes and it finds each by the same hash; one it has not written
   is passed over and reaches it as any attribute does, since it is not one
   it predicts.

4. **A rotation is eight bytes** wherever a frame crosses (`CFrameD`): the
   three smallest components of its unit quaternion at twenty bits each and
   two bits for which was left out. No turn is exactly no turn; any other is
   right to about three millionths of a radian, a third of a millimetre at
   the end of a hundred-metre beam. **The cell holds the packed rotation**,
   so what is compared, checksummed and sent is the same eight bytes on both
   ends, and no machine's arithmetic is inside the checksum. The error
   budget protocol 1 asked for is measured: every prediction, rollback and
   acceptance test passes with it, and a replica's own simulation is
   corrected by position, at a centimetre.

   **The position is not quantised.** A stopped character is held to the
   authority's place to a millimetre, and three doubles are what say it.

5. **A frame whose rotation is the baseline's sends its position alone.** In
   a record that is not whole, bit 13 of a frame field's id says so and 24
   bytes follow where 32 would. Most of what moves walks without turning.

6. **A tick of input is said once a message.** The redundancy stays: each
   message carries the newest tick and the three before it. The first is
   written whole; each after says how many ticks on it is in a byte, and --
   when its intents are those of the tick before it -- says so in two more.
   An intent is its action's number, a byte for its type, whether it is
   pressed and which of its axes are not zero, and those axes.

7. **`GetStats` says where the bytes go**: `SnapshotBytes`, `AttributeBytes`,
   `RemoteBytes`, `UnreliableBytes` and `InputBytes` beside `SwarmBytes`,
   each sent and received together since the session began. What is left of
   the total is instances coming and going, the ground and the handshake.

## Consequences

- **Measured**, the same run, the friend's machine, KB a second:

  | | attributes | snapshots | swarm | in all | sent by the friend |
  |---|---|---|---|---|---|
  | before | 24.0 | 9.1 | 5 to 8 | 41 | 7.9 |
  | after | 1.45 | 6.3 | 6 to 7 | 16 to 17 | 5.4 |

  And in a scene of the engine's own -- a hero the server walks, one the
  player walks, three hundred props that never move:

  | | before | after |
  |---|---|---|
  | an attribute that changes every send | its owner's whole set: 137 bytes for twelve | 24 bytes, and 12 for each more of that owner's |
  | a body walking, a snapshot | 84 bytes | 48 |
  | a client standing still, a second | 6.9 KB | 1.9 |
  | a client walking, a second | 6.9 KB | 2.2 |

- **Protocol 40.** A peer of 39 is refused at the handshake, by name, as
  every protocol change is. A game moves all its players to the package at
  once.

- **An attribute's change is ordered, not idempotent.** An edit lost would
  leave a replica wrong for good; the channel is reliable, and a replica
  that joins again is sent every owner whole.

- **A replica's rotation is the authority's to three millionths of a
  radian**, not to the bit. Nothing compares them by the bit: the checksum
  is of the packed form.

- **An owned part's frame, sent by its owner, is packed the same way**; an
  authority that applies it and captures it again reads the same eight bytes.
  A rotation that is a scale can no longer be sent at all.

- **What is still there to take**: a moving body's three doubles of position
  (24 bytes; a delta from the baseline in fewer bytes would be exact and
  smaller), and its velocity. Not done: neither was most of the cost, and
  each is its own measurement.

- Tests (`engine/replication/tests`):
  - D549: one attribute changing every send costs at most 24 bytes a send
    for an owner of twelve attributes and twelve tags (292 before); a tag
    put on or taken off its own name; an attribute removed, and one of
    another kind beside it; nothing changed, nothing sent.
  - a rotation crosses in eight bytes, no turn exactly, any other to six
    millionths an element, and what is read back packs to the same cell.
  - a body that moves without turning sends 34 bytes a snapshot over a quiet
    one, turning 42, and walking again with the turn it has 34.
  - a client's input held the same is at most 28 bytes a tick at rest and 36
    held over (110 before), a press in the middle of it is heard, and input
    that changes every tick arrives whole.
  - every prediction, rollback and acceptance test that was there.
