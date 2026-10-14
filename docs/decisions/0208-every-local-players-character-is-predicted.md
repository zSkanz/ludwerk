# 0208 — Every local player's character is predicted

- Status: accepted; built (protocol 51)
- Date: 2026-10-12
- Builds on: ADR 0076 (prediction and its correction), ADR 0133 (a replica
  steps its own island, and steps it again), ADR 0205 (several players at one
  screen; "Not decided here" named this), ADR 0163 (other machines'
  characters as stand-ins)

## The question

ADR 0205 gave one machine up to four players. On the machine that hosts, all
four characters are the authority's own and answer their sticks at once. On a
machine that **joined**, only the first player's did: prediction was one
character's from end to end -- one owned id, one history of predicted places,
one drawn offset, and a rollback that put the simulation back and stepped it
again with one stream of input. A guest's character was drawn from the
authority's snapshots like a stranger's, a round trip behind its own stick.

Measured before this, over a link of five frames each way (a sixth of a
second there and back): the first player's character moved two frames after
its stick, the guest's **sixteen**. With that fixed and nothing else, a guest
walking for two seconds covered 13.6 m where the first player covered 15.87:
it answered at once and was then held the round trip behind, written over by
every snapshot.

## The decision

### 1. Who is predicted

On a replica, the character of **every local player the authority has given a
number**: the machine's first player and up to three guests. A guest still
waiting for its number (`scene::ProvisionalUserIdBase` and above) is followed
from snapshots, as before -- the authority does not yet know whose intents
they are.

The session keeps what it kept for one character **a seat**: the net id, the
history of predicted places, the same for a character on the plane,
the authority's word on its predicted attributes, and the drawn offset. Seat
0 is the machine's first player; 1 to 3 are its guests, as protocol 48
numbers them.

### 2. One answer for all of them

Unchanged, and what makes the rest simple: a machine's input is one stream
(protocol 48), and the authority deals each tick of it by seat with **one
intent tick**. So one acknowledged tick answers every character of a machine,
and they are always compared and stepped again from the same moment.

### 3. The rollback steps all of them

The island remembered after the answered tick is restored once. Every local
character is put at the authority's word. Each unanswered tick is then taken
again as **one step of the simulation**, in which each character moves by its
own command of that tick, and the predicted step runs for each with its
own player's intents, in id order.

`ICharacterReplay::replayAll` takes the characters together
(`CharacterReplayStep`: a character, where it starts, its commands);
`replay` is `replayAll` of one.

### 4. One wrong, one right

Each character is compared with what was predicted for it. If any one
differs -- or a part they predict, or an attribute -- **all are restored and
stepped again**: they stand in one island, and half a step is not a step.

The one that was right comes out where it was. So only the wrong one's drawn
offset moves, a correction is counted for the character that was wrong and
for no other, and the log line for corrections says whose it was
(`--net-log-corrections`: "Corrected at tick N, local player 2, ...").

**Where no character was wrong and a part they predict was**, the
correction is the machine's and no player's: it is in `Corrections` and
`LastCorrection`, in nobody's entry, and the log says so in a line of its
own ("no character of this machine's ... a part it predicts was N m off").
As first built it was given to the first character compared, and the first
picture taken of the rows a player showed a crate a quarter of a metre off
as "Player 1: last correction 21.0 cm".

### 5. A guest who sits down, and one who gets up

**Sits down in the middle of a match.** Its seat has a character from the
roster that names it for a number, and the first answer about it is *taken*,
not compared: there is no prediction of it at the tick answered. It is then
stepped forward with the others -- and for the ticks before its own history
begins it is stepped **standing**. This machine sent no intent of its before
its number arrived, so the authority stepped it those ticks with nobody at
its stick, and standing is what the authority did.

That last part was found by the test written for it. Stepping all of
them again wanted as many ticks from each, a guest newly seated had fewer,
and so nobody was stepped: the guest was taken where the authority had it a
round trip ago, walked on from there, and was corrected by the round trip's
walk -- **1.47 m** at eight metres a second over this link -- at the first
answer that could be compared. A guest's first sight of its own character
was a jump back.

**Gets up.** Its seat is cleared with the roster. An island remembered while
it sat names its character; if that character has since left the world, the
island is not restored -- every part it holds is looked for first, and
nothing is touched if one is missing -- and the characters that are left are
stepped alone from the authority's word, as before ADR 0133, until the
answers reach ticks remembered without it: a round trip. A character the game
leaves standing after its player has gone is in the island still and is
restored with it; it is nobody's here any more and follows the authority.

### 6. What they predict, and who stands in

Loose parts (ADR 0133) are predicted within reach of **any** local character,
under the machine's one cap. Other machines' characters stand in (ADR 0163)
against **every** local character, not the first alone.

### 7. The wire: protocol 51

A snapshot's header carried the predicted attributes of the peer's first
player's character. It now carries, after them, a count of guest blocks and
for each a seat, a count and the same entries: what each guest's predicted
steps wrote, as the authority has it after the tick it answers. A seat with
none is not written, so a machine with one player -- nearly every machine --
pays one byte a snapshot.

Without it a guest's predicted attributes were answered by nobody: its
`Attributes` copy is not applied to a character the machine predicts, so a
value the client's own code wrote wrongly stayed wrong for good.

### 8. Where a person reads it

- The overlay's Network panel adds two rows a player -- corrections a second,
  the last correction -- when the machine predicts more than one character.
- The frame report (`--frame-report=<seconds>`) adds a line then: each player's count
  since the session began and how far its last correction moved it.
- `NetworkService:GetStats().LocalPlayers`: one entry for each predicted
  character, with its player's `UserId`, `Corrections`,
  `CorrectionsPerSecond` and `LastCorrection`. A game can say which of two
  people at one screen the link is treating badly.

### 9. What does not change

A host and a solo game: there is no replica, and nothing here runs. A machine
with one player: seat 0 is what the single owned character was, the wire
costs a byte, and its tests -- the hundred corner jumps, the crates, the dash
at 165 ms, the netcode acceptance gate -- pass as they did.

## What it costs

Stepping the island again is **one step a tick however many characters this
machine predicts**: ten corrections over a link of five frames each way were
ten replays of thirteen ticks each, with one character and with four.
Measured in the development build, in one process: about 0.58 ms a replay
with one character, 0.56 to 0.69 with two, 0.71 to 0.73 with three, 0.67 to
0.83 with four -- each character adds its controller's sweep to a step that
is mostly the island's.

## How it is held

In one process (`engine/app/tests/network_session_tests.cpp`, "ADR 0208"),
over a link of five frames each way:

- the guest answers its stick when the first player does (2 and 2 frames;
  16 before);
- two seconds walked are the same two seconds for both (15.87 m each, no
  frame back, no correction; 13.6 m before);
- the authority's word a metre off for one of two, either one: that one is
  corrected once by the metre, and the other is not counted, not slid, and
  walks on at the step it was taking;
- what a guest's predicted step wrote is answered a seat at a time: a dash
  each, no correction; the guest's dash written with no end by the client's
  own code is put right (seen to fail without the guest's block);
- one step a tick for one to four characters, with the cost printed;
- a guest who gets up with a correction in flight, its character taken or
  left standing, and one who sits down in the middle of a walk: the first
  player's character is not moved by either, and the new guest is not
  corrected for having arrived.

In two processes over the real transport (`netcode_acceptance_guest`, the
acceptance gate's bot with a guest at its machine): at 0, 50, 150 and
300 ms, each of the two characters by itself takes no correction on a
straight walk, stops where the authority stopped to a millimetre, and
answers its stick in the same tick; and with the authority's word a metre
off for the guest alone, the guest is corrected once by that metre and the
first player's character is corrected no times, is never aside, and steps
0.1333 m every tick across it.

## Not decided here

- **A listen host's own guests** were never in question: they are the
  authority's.
- **What a machine is sent** is already asked from each of its players
  (protocol 48): the streaming radius and relevance by sight take a
  guest's place as well as the first player's. Nothing here changes it.
- **A character on the plane** is kept a seat like the others, and
  corrected by the error carried forward as before; it has no island to step
  again, with one player or with four.
