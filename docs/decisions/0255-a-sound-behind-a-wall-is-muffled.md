# 0255 — A sound behind a wall is muffled

- Status: accepted
- Date: 2026-10-20
- Builds on: ADR 0009 (miniaudio is the seam), ADR 0131 (sound effects, a
  low-pass among them), ADR 0147 (the quality level), ADR 0205 (an ear a view)

## The question

A sound on a part is heard by how far it is from the listener and on which
side, and by nothing else: a generator in the next room is as clear as one
in front of the player, and a voice through a wall as one across a table. A
game that wants a muffled room has to put a `LowPassSoundEffect` on each
sound and move its cutoff from a script that casts its own rays -- every
game, again, and each a little differently.

## Decision

### 1. What stands between an ear and a sound

Each frame, after the ticks, the engine asks how much of each positional
sound -- a `Sound` under a `BasePart` -- reaches the ear that hears it (ADR
0205: the nearest one at this machine). Five rays go from the ear: to the
part's centre, and to four points around it across the line between them,
half the part's largest extent away (at least 0.1 m, at most 1.5 m). A ray
that meets a part before it reaches its point is blocked; it goes on past
that part once, and a ray blocked twice counts half again. The sound's amount
is the mean of its five: a wall that hides some of its rays muffles it by
that share.

What blocks is what a ray of the physics world meets: a part that can be
queried, terrain, blocks. Characters do not -- a player crossing between a
listener and a sound does not muffle it -- and neither does the part the
sound is on or anything in the model it belongs to: a sound in a car's
engine is not muffled by the car's body.

### 2. A budget in rays

A frame casts at most `AudioService.OcclusionRays` rays. The sounds are
visited in turn: one never visited yet first, then the one whose loudness
here times the frames since its last visit is greatest, so near and loud
sounds come round most often and a quiet one is still visited. Its five rays
give the sound its new amount; between visits the amount it is muffled
moves towards that over about 100 ms, so a sound passing behind a pillar
closes and opens smoothly and a sound visited every fifth frame is not
heard stepping. A sound first visited takes its amount at once: a sound that
starts behind a wall starts muffled.

`OcclusionRays` is the game's to set. Until it does, it follows the quality
level (ADR 0147): 20 on `Low`, 64 above -- about four and about twelve
sounds a frame. Zero casts nothing and every sound is heard open.

### 3. What muffled sounds like

A sound muffled by an amount `m` -- 0 open, 1 behind one wall, 1.5 behind
two or more -- goes through a low-pass at `20000 * (OcclusionMuffle /
20000) ^ m` Hz and has its volume multiplied by `OcclusionVolume ^ m`. The
defaults are 900 Hz and 0.5: a wall takes the brightness and half the level
off, a second wall more of both. `OcclusionEnabled` turns it off for the
game.

### 4. Each machine's, and none of the world's

It changes only what this machine's speakers do. Nothing of it is in the
world's hash, on the wire, in a save or readable by a script; each machine
casts from its own ears, and a server with no listener casts nothing. The
four `AudioService` properties are presentation, outside the world's hash:
a script on each machine sets them for that machine.

## Script surface

- `AudioService.OcclusionEnabled` (bool, `true`)
- `AudioService.OcclusionMuffle` (number, Hz, `900`, 60 to 20000)
- `AudioService.OcclusionVolume` (number, `0.5`, 0 to 1)
- `AudioService.OcclusionRays` (number, whole, 0 to 1024; reads the quality
  level's until written)

A flag on one `Sound` to leave it open is not here: a `Sound`'s properties
travel on the wire, and it goes with ADR 0256's protocol.

## Proof

- The mixer: a muffled voice loses its energy above the cutoff, and its
  level by the factor; an open one is unchanged.
- The world: a sound in the open reads 0, behind a wall 1, behind two walls
  1.5, and behind a wall to its right and one above it 0.4 -- two rays in
  five; a character between does not muffle it; the sound's own part and its
  model do not.
- The budget: with more sounds than a frame's rays, every sound is visited,
  the loudest most often, and a frame casts no more than its budget.
- The cost: 64 sounds measured on the desktop, on the standalone headset
  profile and on Android, shown in F3.

## Not decided here

- **What a wall is made of**: glass lets more through than concrete. A
  material's transmission is a later tier.
- **Sound bending round a corner**: a door left open is heard through the
  door, not through the wall; diffraction and paths between rooms are a
  later tier, with rooms and portals.
- **A room's own sound**: reverberation by the room a sound is in is a later
  tier; `ReverbSoundEffect` on a group is what a game uses now.
