# 37 — Character

A character that is animated the way a game's is (ADRs 0197, 0198, 0199,
0201): three heroes of three builds, on one library of clips none of them
carries, mixed by one animation graph with no game code doing the mixing --
and, on top of the clips, feet that find the stairs, heads that look at each
other, a staff held in two hands, a crossbow whose string the reload draws,
and a hood that comes off.

Run it with `run.bat`, or `engine-host examples/37-character`.

- **W** runs forwards, **S** backs away, **A** and **D** run to the side and
  somewhat ahead -- as the hero faces, which **Q** and **E** (or the arrow
  keys) turn.
- **Shift** walks. **Space** jumps.
- **F** or the left mouse button shoots, and the reload follows.
- **G** hands the crossbow to the other hand. **H** changes the head.

## What to look at

- **The rogue is yours.** Stand, walk, run, strafe, back away, jump off the
  platform, shoot while running: every one of those is the graph mixing clips
  from how the body moves. The scripts never name a clip.
- **Three builds, one library.** The rogue is the body the clips were made
  on. The skeleton is another character on the same rig, made taller and
  thinner. The mannequin is another pack's body altogether: other names for
  its joints, another rest, legs more than twice as long. All three play
  `clips/adventurer.glb`.
- **The skeleton's feet** on the stairs: one on a step and the other on the
  next, the hips let down between them, and on the ramp the soles lying along
  it. Its capsule only knows that it went up.
- **The heads.** The skeleton and the mannequin watch yours; yours watches
  whichever of them is nearer -- and the head that turns is a piece the body
  wears.
- **The staff.** It is welded to the skeleton's right hand, which swings as
  the walk swings it; the left hand is put on the staff, wherever that is.
- **The crossbow.** It hangs from a `Bone` on the rogue's hand, and its string
  is a joint that the shot lets go and the reload draws back -- moved by the
  clip that moves the arms. **G** puts it in the other hand.
- **The hood.** The rogue's head is a mesh of its own on the body's skeleton.
  **H** takes it off and puts on the same head with no hood.
- **The mannequin** slashes every few seconds and leans back and straightens
  again: an attribute the server sets, and a number it eases.
- **`steps:`** at the top of the screen counts the moments the graph names in
  the walk and the run, as your feet land.

## How it works

**The graph** (`content/anim/hero.animgraph.json`). A `Body` layer has one
state for everything on the ground -- a blend across two parameters, how fast
the body goes sideways and how fast forwards, with the idle in the middle, the
walk and the run ahead of it, the walk back behind and a strafe to either
side, each at the speed its clip covers, all on one phase so the feet of one
keep time with the feet of the next -- and a jump and a fall it goes to when
the body leaves the ground going up, or has been falling. An `Arms` layer,
masked from the chest, plays the slash, or the shot and then the reload, over
whatever the legs are doing. A `Lean` layer adds a lean to all of it. Its
parameters are read from the world: the `CharacterBody`'s speed and whether it
is on the ground, and three attributes of the body. `Attack` and `Shoot` are
triggers, and a trigger read from an attribute fires when the value changes,
so the server writes a counter. That is why there is nothing to replicate:
every machine has the body and its attributes already, and steps its own copy
of the graph.

**Retargeting.** Each joint of a rig is given a role -- hips, left upper arm,
right foot -- from its name, and the mannequin's names are not the library's.
A clip is carried across as each joint's turn from rest, through each rig's
own rest pose, so a joint drawn along another axis turns the same way in the
world; the difference between two stances is taken out once; the hips' travel
is scaled by the length of the legs; and every other length is the body's
own. Nothing was done for it but naming the joints as rigs are named. The mask
of the `Arms` layer says `Chest`, a role, so it is the right joint on all
three.

**A speed is read in the library's strides.** The clips were made on short
legs: the library's walk covers 0.54 metres a second on them. The skeleton's
legs are longer, the same walk covers 0.62 on it, and 0.62 is how fast the
server walks it -- and what its graph reads is 0.54, the walk, because a
body's speed over the ground reaches a graph divided by how much longer its
legs are than the library's. The blend's numbers are the library's, once, for
every body.

**Feet** (`FootPlacement` under each model). Each frame a ray down from each
ankle finds the ground. The foot is put on it, as high above it as the clip
had the foot above the floor; the hips come down by what the lower foot
needs; the knees bend to suit. It is picture: done on each machine for the
frame it draws, and nothing in the tick reads it.

**The look** (`IKControl`, `Type = LookAt`, under each model). The head, and
as many joints above it as `ChainLength` says, turn to face the target and
stop at `MaxAngle`. The target here is a `Bone` between another hero's eyes.

**The staff.** A `Bone` on the pack's hand slot -- the joint its characters
hold things by -- a `Weld` from it to the staff, an `Attachment` further up
the staff, and an `IKControl` that puts the left hand on that attachment. A
`Pole` says which way the elbow goes. The staff collides with nothing
(`CanCollide` off), and the grip is found where the frame DRAWS the staff: on
a stair the feet lower the whole body, the right hand and the staff go down
with it, and the left hand is still on the grip.

**What is worn and what is held** (`MeshPart.PoseFrom`). A mesh that names
another in `PoseFrom` is not posed from clips: its joints are the other's,
joint for joint, whatever moved them -- a clip, the look, the feet on a stair.
The rogue's head is such a piece: a file with the body's skeleton and one
mesh. Changing it is destroying one `MeshPart` and parenting another, at run
time, and the frame after it is on the body.

The crossbow has two joints of its own, `Crossbow` and `String`, and none of
the body's, so nothing says where it is -- until it is parented under a
`Bone` of the body, and then it hangs from that joint. `String` is moved by
the track of that name in whatever clip the body is playing: the library's
rig has a `String` joint under its hand, and the shot and the reload each key
it. One `AnimationPlayer`, one graph, and the prop is in time with the arms
because it is the same clip. Handing it over is parenting it under the other
hand's `Bone`.

## The files

- `content/clips/adventurer.glb` -- the library: one rig and twelve clips.
- `content/models/rogue.glb`, `rogue_hood.glb`, `rogue_head.glb` -- the
  player's body and its two heads.
- `content/models/skeleton.glb`, `content/models/mannequin.glb` -- the other
  two. No clip in any of them.
- `content/models/crossbow.glb` -- drawn by `tools/make_content.py`.
- `content/THIRD_PARTY.md` and `content/licences/` -- whose work the models
  and clips are, and under what terms: all of it CC0.
- `content/anim/hero.animgraph.json` -- the graph, written by hand.
- `content/scenes/character.scene.json` -- the level and the heroes, written
  by `tools/make_scene.py`: how a hero is put together is at its top. Open
  the project in the editor and all of it is there.
- `src/server/init.luau` -- who walks where: the player's hero from what the
  keys say, the skeleton round its route, the mannequin's attributes; and
  what the rogue wears and holds.
- `src/client/init.luau` -- the keys, the camera, and the count of steps.
- `tools/make_content.py` -- cuts the models and the library out of the packs
  they come from, with `tools/gltf_edit.py`. It needs the packs; running the
  example does not, and nothing is fetched to build it.

## What is not in it

- **A match.** It is one machine. It is written as a match needs it -- the
  server walks, the client only reads keys, nothing plays a clip -- but one
  hero is given to the first player, and nobody else gets one.
- **Root motion.** The clips walk on the spot and the `CharacterBody` moves
  the hero, so a foot can slide a little where the pace is not a clip's own:
  between the walk and the run, and stepping straight to the side, which the
  library has no clip for -- its strafe is a run to the side and ahead.
- **A walk made for long legs.** The library's is a short body's, with its
  feet lifted high; carried to the mannequin's legs it is a march. Carrying a
  clip across keeps its angles, not its character.
- **A bolt.** The string moves; nothing leaves the crossbow.
- **Hands that find their own grip, or a body that leans to reach**: a limb,
  a look and two feet are what there is.
