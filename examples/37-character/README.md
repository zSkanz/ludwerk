# 37 — Character

A character that is animated the way a game's is (ADRs 0197, 0198, 0199):
three heroes of three builds, on one library of clips none of them carries,
mixed by one animation graph with no game code doing the mixing -- and, on
top of the clips, feet that find the stairs, heads that look at each other and
a staff held in two hands.

Run it with `run.bat`, or `engine-host examples/37-character`.

- **W** runs forwards, **S** backs away, **A** and **D** step sideways -- as
  the hero faces, which **Q** and **E** (or the arrow keys) turn.
- **Shift** walks. **Space** jumps. **F** or the left mouse button slashes.

## What to look at

- **The blue one is yours.** Stand, walk, run, sidestep, back away, jump off
  the platform, slash while running: every one of those is the graph mixing
  clips from how the body moves. The scripts never name a clip.
- **Three builds, one walk.** The blue hero is tall and thin, the orange one
  has short legs and arms to its knees, the green one is the size of a child.
  Each is another file from another "tool", with its own joint names and its
  own bone lengths, and the green one rests in another stance. They all play
  `clips/humanoid.gltf`.
- **The orange one's feet** on the stairs: one on a step and the other on the
  next, the hips let down between them, and on the ramp the soles lying along
  it. Its capsule only knows that it went up.
- **The heads.** The orange and the green hero watch yours; yours watches
  whichever of them is nearer.
- **The staff.** It is welded to the orange hero's right hand, which swings as
  the walk swings it; the left hand is put on the staff, wherever that is.
- **The green one** slashes every few seconds and leans back and straightens
  again: an attribute the server sets, and a number it eases.
- **`steps:`** at the top of the screen counts the moments the graph names in
  the walk and the run, as your feet land.

## How it works

**The graph** (`content/anim/hero.animgraph.json`). A `Body` layer has one
state for everything on the ground -- a blend across two parameters, how fast
the body goes sideways and how fast forwards, with the idle in the middle, the
walk and the run ahead of it, the walk back behind and a sidestep on either
side, all on one phase so the feet of one keep time with the feet of the next
-- and a jump and a fall it goes to when the body leaves the ground going up,
or has been falling. An `Arms` layer, masked from the chest, plays the slash
over whatever the legs are doing. A `Lean` layer adds a lean to all of it. Its
parameters are read from the world: the `CharacterBody`'s speed and whether it
is on the ground, and two attributes of the body. `Attack` is a trigger, and a
trigger read from an attribute fires when the value changes, so the server
writes a counter. That is why there is nothing to replicate: every machine has
the body and its attributes already, and steps its own copy of the graph.

**Retargeting** (`tools/make_heroes.py`). Each joint of a rig is given a role
-- hips, left upper arm, right foot -- from its name, and the three heroes use
three families of names. A clip is carried across as each joint's turn from
rest, through each rig's own rest pose, so a joint drawn along another axis
turns the same way in the world; the difference between two stances is taken
out once; the hips' travel is scaled by the length of the legs; and every
other length is the body's own. Nothing was done for it but naming the joints
as rigs are named. The mask of the `Arms` layer says `Chest`, a role, so it
is the right joint on all three.

**Feet** (`FootPlacement` under each model). Each frame a ray down from each
ankle finds the ground. The foot is put on it, as high above it as the clip
had the foot above the floor; the hips come down by what the lower foot
needs; the knees bend to suit. It is picture: done on each machine for the
frame it draws, and nothing in the tick reads it.

**The look** (`IKControl`, `Type = LookAt`, under each model). The head, and
as many joints above it as `ChainLength` says, turn to face the target and
stop at `MaxAngle`. The target here is a `Bone` between another hero's eyes.

**The staff.** A `Bone` on the right hand, a `Weld` from it to the staff, an
`Attachment` further up the staff, and an `IKControl` that puts the left hand
on that attachment with the attachment's turn (`AlignRotation`). A `Pole`
says which way the elbow goes. The staff collides with nothing
(`CanCollide` off), and the grip is found where the frame DRAWS the staff:
on a stair the feet lower the whole body, the right hand and the staff go
down with it, and the left hand is still on the grip.

## The files

- `content/clips/humanoid.gltf` -- the library: a rig and ten clips, written
  by `tools/make_clips.py`, which is also how the clips are made: where a
  foot is at each moment of a step, and the leg bent to reach it.
- `content/models/tall.gltf`, `stocky.gltf`, `small.gltf` -- the heroes,
  written by `tools/make_heroes.py`. No clip in any of them.
- `content/anim/hero.animgraph.json` -- the graph, written by hand.
- `content/scenes/character.scene.json` -- the level and the heroes, written
  by `tools/make_scene.py`: how a hero is put together is at its top. Open
  the project in the editor and all of it is there.
- `src/server/init.luau` -- who walks where: the player's hero from what the
  keys say, the orange one round its route, the green one's attributes.
- `src/client/init.luau` -- the keys, the camera, and the count of steps.
- `tools/rig.py` -- what the model scripts share: a rig, boxes, a glTF.

## What is not in it

- **A match.** It is one machine. It is written as a match needs it -- the
  server walks, the client only reads keys, nothing plays a clip -- but one
  hero is given to the first player, and nobody else gets one.
- **Root motion.** The clips walk on the spot and the `CharacterBody` moves
  the hero, so a foot can slide a little where the pace is not the clip's.
- **Clips worth keeping.** They are sums in a script, there to be read.
- **Hands that find their own grip, or a body that leans to reach**: a limb,
  a look and two feet are what there is.
