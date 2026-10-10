# 0197 — Animation graphs: states, transitions and blends, with no game code doing the mixing

- Status: accepted; built in the animation batch of the level-up roadmap
- Date: 2026-10-10
- Decided by: the owner (the roadmap's first category: "a state machine with
  blending -- walk, run and attack mixing with no game code"); shaped by the
  orchestrator (parameters bound to what replicates, a blend along two
  parameters, additive layers and events in scope, root motion out; and, when
  the proof below failed, `AnimationPlayer` on the wire as protocol 44);
  detailed by the agent from the code
- Builds on: the skeletal animation system (M6) and its tracks, H3 (a pose
  is built as often as its mesh is seen), H10 (bodies on one rig, one clip
  and one moment share a pose), ADR 0090 (an asset that is a JSON file),
  ADR 0106 (attributes replicate), R10

## The question

An `AnimationPlayer` plays clips. Everything else a character needs is the
game's code today: which clip, when to start it, how long to fade, how much
of a walk and how much of a run at this speed, how to keep an attack on the
arms while the legs go on walking, when the sword's edge is live. Every game
writes that, and writes it slightly wrong -- a fade that pops, feet that
slide between a walk and a run of different lengths.

## What is there

Read before this was written. Tracks: a clock, a speed, a loop, a weight
with a fade to a target. Tracks are mixed as a weighted average a joint a
component, **normalised by the total weight** -- one track at any weight
above nought owns the joints it has channels for, and there is no "part of
this over that". No layers, no masks, no blend by a parameter, no time
shared between clips, no state, no event but a clip's end, and no asset but
the clips inside a model. **Nothing of animation is replicated**:
`AnimationPlayer` is out of the wire's schema, and a clip plays on the
machine whose script played it.

## Decision

An **animation graph** is an asset, `*.animgraph.json`, that says how a
character's clips are mixed; an `AnimationPlayer` given one plays it, and
what drives it is **parameters** -- a speed, whether it is on the ground, a
trigger -- that the game sets, or that the graph reads from the world.

```json
{
  "format": "animgraph",
  "version": 1,
  "library": "asset://clips/humanoid.glb",
  "parameters": {
    "Speed":    { "number": 0,     "from": "CharacterBody.Speed" },
    "MoveX":    { "number": 0,     "from": "CharacterBody.MoveX" },
    "MoveZ":    { "number": 0,     "from": "CharacterBody.MoveZ" },
    "Grounded": { "boolean": true, "from": "CharacterBody.Grounded" },
    "Attack":   { "trigger": true, "from": "Attribute.Attack" },
    "Aim":      { "number": 0 }
  },
  "events": {
    "Slash": [ { "at": 0.35, "name": "Hit" } ],
    "Run":   [ { "at": 0.0, "name": "Step" }, { "at": 0.5, "name": "Step" } ]
  },
  "layers": [
    {
      "name": "Body",
      "start": "Move",
      "states": {
        "Move": { "blend": "Speed", "sync": true,
                  "clips": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1.6 }, { "clip": "Run", "at": 5 } ] },
        "Strafe": { "blend": [ "MoveX", "MoveZ" ], "sync": true,
                    "clips": [ { "clip": "Idle", "at": [0, 0] }, { "clip": "Forward", "at": [0, 1] },
                               { "clip": "Back", "at": [0, -1] }, { "clip": "Left", "at": [-1, 0] },
                               { "clip": "Right", "at": [1, 0] } ] },
        "Jump": { "clip": "Jump", "loop": false },
        "Fall": { "clip": "Fall" }
      },
      "transitions": [
        { "from": "Move", "to": "Jump", "when": [ ["Grounded", "==", false] ], "fade": 0.1 },
        { "from": "Jump", "to": "Fall", "after": 1.0, "fade": 0.15 },
        { "from": "*", "to": "Move", "when": [ ["Grounded", "==", true] ], "fade": 0.2 }
      ]
    },
    {
      "name": "Arms",
      "mask": [ "Spine1" ],
      "start": "None",
      "states": { "None": {}, "Slash": { "clip": "Slash", "loop": false } },
      "transitions": [
        { "from": "*", "to": "Slash", "when": [ ["Attack"] ], "fade": 0.05 },
        { "from": "Slash", "to": "None", "after": 1.0, "fade": 0.2 }
      ]
    },
    {
      "name": "Lean",
      "additive": true,
      "weight": "Aim",
      "start": "Up",
      "states": { "Up": { "clip": "AimUp" } }
    }
  ]
}
```

### What a graph is made of

- **Clips**, named as `LoadAnimation` names them: a plain name is a clip in
  the graph's `library` -- one file every character on the graph shares --
  or, with no library, in the mesh the player drives; a name with a `#`
  says its own file. A library is read because a graph names it, though no
  mesh wears it.
- **Parameters**: a number, a boolean or a trigger. Each has its value at
  rest and may say where it comes `from` when nothing has set it:
  - `CharacterBody.Speed` -- how fast the `CharacterBody` the player belongs
    to moves over the ground; `.VerticalSpeed`; `.MoveX` and `.MoveZ`, its
    velocity over the ground in its own frame, right and forward;
    `.Grounded`; `.State`. **The speed is read from where the body was a
    tick ago**, settled over three ticks: a position is the one thing every
    machine has of a character, and a replica is told another player's in
    steps.
  - `Attribute.<Name>` -- an attribute of the player's parent. One nobody
    has set leaves the parameter at rest.
  - **The tick a body is first seen, its parameters keep their rest.** A
    body says whether it is on the ground after the physics has stepped it,
    and a graph is stepped before the physics: a character just made says
    "not on the ground" of the floor it stands on, and the plainest rule
    there is -- in the air is a jump -- played a tick of the jump for every
    character that appeared.
  - **A trigger with a source fires when the source changes** -- an
    attribute set to anything it was not. Without one it fires when a
    script sets it. Either way it is true until a transition takes it, or
    the tick ends.
- **Layers**, bottom first. A layer has a **mask** -- joints, each with
  everything below it, named as the file names them or by their role (ADR
  0199), so one graph is masked the same on rigs that call their spines
  different things -- or none, which is the whole rig; and a **weight**, a
  number or a parameter's name.
  - An ordinary layer **replaces**: where it has a pose it takes its weight
    of the joints it masks, and what is under it keeps the rest. A state
    with no clip is "nothing", and the layer under shows through.
  - An **additive** layer **adds**: each clip is taken as how far it is
    from its own first frame, and that difference is put on top of whatever
    the layers under it made -- a lean, a breath, an aim up and down, a
    flinch over a run.
- **States**: a clip; a **blend** of clips along one parameter, each `at` a
  value, the two either side mixed by how far between them the parameter
  is; or a blend across **two** parameters, each clip `at` a point, the
  three round the parameters' point mixed by where it lies between them
  (the points are triangulated when the graph is read; outside them, the
  nearest edge). `sync` gives a blend's clips one phase, so a walk and a
  run of different lengths keep their feet together while one becomes the
  other, and the blend plays at the rate their mix gives. A state may
  scale its rate (`speed`) and say it does not loop (`loop: false`).
- **Transitions**: from a state (or `*`, any) to a state, taken when every
  condition in `when` holds -- a parameter against a value, or a trigger
  alone -- and, with `after`, not before that much of the state's clip has
  played (1 is its end, which a clip that loops never reaches: `after: 1`
  is for a state that does not). `fade` is the seconds the two are
  cross-faded over.
  In the file's order; the first that holds is taken. A fade under way can
  be left: what was fading out goes on fading out beside the new one.
- **Events**: named moments of a clip, by how far through it they are. An
  event fires when the clip passes it in the state a layer is in -- not in
  one fading out, and in a blend only for the clip that weighs most, so a
  walk and a run do not step twice.

### The API

- `AnimationPlayer.Graph: Content` -- the graph. Empty is a player as it was.
- `AnimationPlayer:SetParameter(name, value)`, `GetParameter(name)`,
  `ClearParameter(name)`, `GetState(layer?) -> string`.
- `AnimationPlayer.StateChanged(layer, from, to)` and
  `AnimationPlayer.EventReached(name, layer)`, on the simulation's clock,
  drained where a track's `Ended` is. **An event on a clip fires once, and
  not again on a resimulation**: a predicted tick that is run again does not
  step the graph (below), so a footstep or a hit is not heard twice.
- `LoadAnimation` goes on working beside a graph: a track a script plays is
  mixed with the graph's pose by its weight, as two tracks always were.

**`SetParameter` on a parameter that has a source overrides the source** on
the machine that called it, until `ClearParameter` hands it back -- the rule
a morph target's weight has (ADR 0196). It raises for a name the graph does
not declare, once the graph has loaded; before that it is kept.

### How it is mixed

The graph does not make a pose. **Each tick it makes tracks**: for every
layer, the clips its states are playing, each with a time, a weight and the
layer's mask -- and the pose walk that exists mixes them. Three things are
added to that walk:

1. **A track's weight may differ a joint**, by a mask: what a layer needs
   to own some joints and not others.
2. **Layers are weighted to replace, not to average**: a layer of weight
   `a` on a joint takes `a`, and what is under it shares the `1 - a` left.
   That is a cross-fade written as weights, so it is still one weighted
   average and the walk does not change shape. The FIRST layer is the old
   average itself -- every track a script plays is in it, beside the
   graph's -- with one thing added: where a graph's first layer is partly a
   state with no clip, the rest pose weighs that part, or a fade in from
   nothing would arrive whole on its first tick.
3. **An additive track is applied after the average**: its translation
   added, its rotation multiplied on, each scaled by its weight, from the
   difference between the clip at its time and the clip at its start.

A blend along one or two parameters and a cross-fade between two states are
weights that add to one, which the walk already does right.

**H3 and H10 hold.** A graph's tracks are (clip, time, weight, mask,
additive) like any other, so a body seen small is still posed every fourth
tick at a quantised time, and a crowd of one rig in one state at one moment
still shares one pose: the mask and the additive flag join the pose's
signature and nothing else does.

### Which side of the hash (R10)

**The simulation's.** A pose is read by the tick -- a `Bone` holds a sword,
a hitbox rides a joint, a ragdoll starts from it -- so what picks the pose
is the tick's too. The graph is stepped where tracks are, at
`PreAnimation`, on the simulation's clock, from parameters a script set in
the tick or that are read from the world's own state. No wall clock, no
frame rate, transitions in file order, events in layer order.

Tracks are not in the world's hash today -- a pose reaches it only through
what reads the pose -- and a graph's state is one step further from it. So
it is **hashed by hand**, as a sprite animator's phase is: for each player
with a graph, each layer's states with their phases and weights, and the
parameters, as one number the host writes on the player after each sample
and the world's hash reads. A graph that diverges between two runs shows at
the tick it diverges, not when a sword ends up somewhere else.

### In a match: the player travels, the animation does not (protocol 44)

**This section was first written as "nothing is added to the wire", and the
proof it named failed.** Two processes, a server and a joiner: a character
the server spawns for a player -- from a stamp, as a `Clone` of a template,
with `Instance.new` -- reached the joiner WITHOUT its `AnimationPlayer`, in
all three ways. A class the wire does not carry is not spawned on a replica
at all; only a character sitting in the scene file would have had one, and
a player's character is never that. So no game could have had its
characters animated on other machines without a client script making the
player -- which is the game code this decision exists to remove.

The fallback the section named was taken, as the orchestrator approved:
**`AnimationPlayer` is on the wire, with `Graph` and `Retargeting`**, sent
when the character is made and when one changes. Nothing else of animation
is sent -- no track, no clip, time or weight, no state, no parameter a
script set -- and nothing a tick. (The same proof found twelve other classes
a spawned thing loses; they are protocol 44 too, and are ADR 0069's
amendment and the multiplayer guide's table, not this decision's.)

One mechanism replicates a character -- its body's state, its attributes --
and the graph is driven by exactly that:

- A parameter with a source is read, **on each machine**, from what already
  arrives: the `CharacterBody`'s speed and whether it is on the ground, an
  attribute. Each machine steps its own copy of the graph from the same
  inputs. The variables are replicated; the animation is not -- which is
  how the major engines do it.
- **A server-side attack is one `SetAttribute`**: a trigger whose source is
  that attribute fires on every machine when the value changes.
- **Somebody who joins late** sees the state the values that arrive put the
  graph in. A trigger fires on a change, and a first value is not one: an
  attack that ended before they joined is not played again.
- A parameter a script sets with `SetParameter` is set on that machine,
  like a track it plays.
- The player itself is part of the character as the server makes it, and
  arrives with it.

What two machines see is the same state from the same inputs, not the same
phase to the tick. Nothing gameplay reads may depend on a REPLICA's graph:
on the machine with authority the graph is simulation, and what a hit needs
of a pose is decided there.

**The acceptance test** is two processes: the host's character walks, runs,
jumps and attacks, and the one who joined sees each, with no `RemoteEvent`
in the game's code and no script on the client that makes or plays
anything.

### Prediction and rewinding

- **A graph is not saved and restored with a predicted world.** A replica
  that predicts its own character resimulates ticks when the server
  corrects it; animation is stepped once a real tick, as tracks are, and a
  resimulated tick does not step it again. A correction reaches the graph
  as its inputs changing -- a speed, a state -- and shows as a fade, not as
  a pose snapped back.
- **There is no lag compensation today.** When a hit is judged against
  where things were, what is rewound is parts, and a hitbox is a part: it
  goes back to where the pose had put it. The pose itself is not rewound
  and nothing needs it to be.

### In the editor

A graph is a file, written by hand in a text editor; a change to it is
picked up as it is saved, as a material's is, and every player on it starts
again from its first states. The content browser knows its kind and gives it
an icon; `Graph` is picked like any content. With a player selected,
Properties shows the graph live -- each layer's state, how far through it is
and what it is fading from, every parameter's value and where it comes from
-- with a control a parameter that sets it as `SetParameter` would, so a
graph is tried and debugged with nothing printed. The engine's own text
editor does not open one: it knows two languages and JSON is neither. A
drawn editor of states and arrows belongs with the material node editor of
the roadmap's fourth category, on the same canvas, and saves this format.

## What this does not do

- **No root motion**: a clip plays in place and the `CharacterBody` moves
  the character. Moving a body by its clip reaches into the body and into
  prediction, and is the known next step with a design of its own.
- **No sub-graphs**: a state is a clip or a blend, not a graph.
- **No events for a track a script plays**: an event is a graph's.
- **No retiming of a transition by a curve**: a fade is linear.

## Proof

Unit tests on the graph alone (states, conditions, triggers and a trigger's
source, `after`, fades, a fade interrupted, a synced blend's phase, the
triangles of a blend across two parameters, events fired once and in
order), on the walk's additions (a masked layer leaves the legs to the
layer under it; weights by layer equal a cross-fade; an additive clip adds
its difference and nothing at its first frame), that a crowd on one graph
still shares poses, and that the hash moves with a graph's state. The
conformance suite holds the API on a player whose graph has not loaded.
`examples/37-character` is a character that idles, walks, runs, strafes,
jumps and attacks from one graph and no code but its input; a determinism
scenario replays it; and the two-process test above is in the gate.

## Amended 2026-10-10: a speed is read in the library's strides (D625)

`CharacterBody.Speed`, `MoveX` and `MoveZ` reach a graph divided by how much
longer the body's legs are than the library's -- the hips scale of ADR 0199's
map, found once a pair of rigs and again when one is replaced. A clip carried
to another build covers ground in proportion to its legs, and a blend placed
in metres a second was wrong on every body but the library's own by that
ratio: the feet slid. The blend's numbers are the library's, once.
`VerticalSpeed` is left in metres: a fall is nobody's stride. A body on the
library's own skeleton has a scale of one and reads what it did. It is on
the simulation's side of the hash and is a function of the two rigs alone.
