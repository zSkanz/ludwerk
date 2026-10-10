# Animation graphs

A character idles, walks, runs, jumps and swings a sword, and something has
to decide which clips play, how much of each, and how one becomes another. An
**animation graph** is a file that says so, and an `AnimationPlayer` given one
does the mixing itself. The game says how fast the character is going; it does
not blend anything.

```luau
--!strict
local animator = Instance.new("AnimationPlayer")
animator.Graph = "asset://anim/hero.animgraph.json"
animator.Parent = hero        -- the model, or the CharacterBody, the hero is
```

`examples/37-character` is three heroes of different builds on one graph and
one library of clips.

## The file

A graph is a JSON file whose name ends in `.animgraph.json`:

```json
{
  "format": "animgraph",
  "version": 1,
  "library": "asset://clips/humanoid.glb",
  "parameters": {
    "Speed":    { "number": 0,     "from": "CharacterBody.Speed" },
    "Grounded": { "boolean": true, "from": "CharacterBody.Grounded" },
    "Attack":   { "trigger": true, "from": "Attribute.Attack" },
    "Aim":      { "number": 0 }
  },
  "events": {
    "Run":   [ { "at": 0.0, "name": "Step" }, { "at": 0.5, "name": "Step" } ],
    "Slash": [ { "at": 0.35, "name": "Hit" } ]
  },
  "layers": [
    {
      "name": "Body",
      "start": "Move",
      "states": {
        "Move": { "blend": "Speed", "sync": true,
                  "clips": [ { "clip": "Idle", "at": 0 }, { "clip": "Walk", "at": 1.6 }, { "clip": "Run", "at": 5 } ] },
        "Jump": { "clip": "Jump", "loop": false },
        "Fall": { "clip": "Fall" }
      },
      "transitions": [
        { "from": "Move", "to": "Jump", "when": [ ["Grounded", "==", false] ], "fade": 0.1 },
        { "from": "Jump", "to": "Fall", "after": 1.0, "fade": 0.15 },
        { "from": "*",    "to": "Move", "when": [ ["Grounded", "==", true] ], "fade": 0.2 }
      ]
    },
    {
      "name": "Arms",
      "mask": [ "Chest" ],
      "start": "None",
      "states": { "None": {}, "Slash": { "clip": "Slash", "loop": false } },
      "transitions": [
        { "from": "None",  "to": "Slash", "when": [ ["Attack"] ], "fade": 0.05 },
        { "from": "Slash", "to": "None",  "after": 1.0, "fade": 0.2 }
      ]
    },
    { "name": "Lean", "additive": true, "weight": "Aim", "start": "Up",
      "states": { "Up": { "clip": "AimUp" } } }
  ]
}
```

A key the format does not have is an error, with the place it was found, so a
typo is not a setting that silently does nothing. A graph that cannot be read
is said in the log -- the file, where in it, and what was wrong -- and its
player plays only the tracks a script loads.

### Clips

A clip is named as `LoadAnimation` names one. A plain name is a clip in the
graph's `library` -- one file of clips that every character on the graph
shares -- or, with no library, in the mesh the player drives. A name with a
`#` says its own file: `"asset://clips/extra.glb#Wave"`.

A library is read because a graph names it: no mesh has to wear it. A clip
from another file is carried onto the character's own skeleton
([Retargeting](manual:animation/retargeting)), which is how one graph and one
library move heroes of different builds.

### Parameters

A parameter is a `number`, a `boolean` or a `trigger`, with its value at rest.
`from` says where its value comes from while no script has set it:

| `from` | What it reads |
|---|---|
| `CharacterBody.Speed` | How fast the body moves over the ground, in metres a second. |
| `CharacterBody.VerticalSpeed` | How fast it rises (negative: falls). |
| `CharacterBody.MoveX`, `.MoveZ` | Its speed over the ground in its own frame: to its right, and forward. |
| `CharacterBody.Grounded` | Whether it is standing on something. |
| `CharacterBody.State` | `Enum.CharacterState` as a number: 0 on the ground, 1 in the air, 2 swimming, 3 flying. |
| `Attribute.<Name>` | An attribute of what the player is parented to. One nobody has set leaves the parameter at rest. |

The body is the `CharacterBody` the player is parented to, or the nearest one
above it, or -- for a player parented to a `Model` -- the first one in the
model. Its speed is read from where it was a tick ago and settles over a few
ticks, which is the one thing every machine has of a character: the one that
walks it, and the one that is only told where it is.

**A speed is read in the library's strides.** A clip carried to a body of
other proportions covers ground in proportion to its legs
([Retargeting](manual:animation/retargeting)): a walk that is 1.5 metres a
second on the body it was made on is 0.75 on legs half as long. So `Speed`,
`MoveX` and `MoveZ` reach a graph divided by how much longer the body's legs
are than the library's -- the short body walking at 0.75 reads 1.5, the walk
-- and the numbers a blend places its clips at are the library's own, once,
for every body that uses the graph. A body on the library's own skeleton
reads metres a second as they are, and `VerticalSpeed` always does.

**A trigger with a source fires when the source changes** -- an attribute set
to anything it was not. Without a source it fires when a script sets it.
Either way it is true until a transition takes it, or the tick ends.

### States

- **A clip**: `{ "clip": "Jump", "loop": false }`. `loop` is true unless it
  says otherwise; `speed` scales the clip's rate.
- **A blend along one parameter**: each clip `at` a value. The two either side
  of the parameter are mixed by how far between them it is; past the ends, the
  end clip alone.
- **A blend across two parameters**: `"blend": ["MoveX", "MoveZ"]`, each clip
  `at` a point `[x, z]`. The three clips round the parameters' point are mixed
  by where it lies between them; outside them all, the nearest edge.
- **`sync`** gives a blend's clips one phase, so a walk and a run of different
  lengths keep their feet together while one becomes the other.
- **`{}`** is a state with no clip. In a layer above the first it is "nothing":
  the layers under it show through.

### Transitions

From a state -- or `*`, any -- to a state, taken when every condition in
`when` holds. A condition is a trigger or a boolean alone, `["Attack"]`, or a
parameter against a value with `==`, `~=`, `<`, `<=`, `>` or `>=`. With
`after`, a transition waits for that much of the state to have played: 1 is
its end, **which a clip that loops never reaches** -- `after: 1.0` is for a
state with `"loop": false`.

They are tried in the file's order and the first that holds is taken, one a
layer a tick. `fade` is the seconds the two states are cross-faded over; a
fade under way can be left, and what was fading out goes on fading out beside
the new state.

### Layers

Layers are mixed bottom first.

- A **mask** is a list of joints, each with everything below it: the layer
  has those joints and no others. A joint is named as the file names it, **or
  by what it is** -- `Chest`, `LeftUpperArm`, a role from
  [Retargeting](manual:animation/retargeting) -- which is what lets one graph
  be masked the same on rigs whose files call their spines different things.
- A layer **replaces**: on its joints it takes its `weight` of the pose, and
  the layers under it keep the rest. `weight` is a number or a parameter's
  name.
- An **`additive`** layer **adds**: each clip is taken as how far it is from
  its own first frame, and that difference is put on top of what the layers
  under it made. A lean, a breath, an aim up and down: make the clip's first
  frame the pose it adds nothing in.

### Events

Named moments of a clip, by how far through it they are. An event fires when
its clip passes it in the state a layer is IN -- not in one fading out -- and
in a blend only for the clip that weighs most, so a walk and a run do not step
twice.

## From a script

```luau
--!strict
animator:SetParameter("Aim", 0.6)        -- on this machine, until it is set again
animator:SetParameter("Attack", true)    -- a trigger: fired
print(animator:GetParameter("Speed"))    -- what the graph sees this tick
print(animator:GetState())               -- the first layer's state: "Move"
print(animator:GetState("Arms"))

animator.StateChanged:Connect(function(layer: string, from: string, to: string)
    print(`{layer}: {from} -> {to}`)
end)
animator.EventReached:Connect(function(name: string, layer: string)
    if name == "Step" then
        footstep:Play()
    end
end)
```

**`SetParameter` on a parameter that has a source overrides the source**, on
the machine that called it, until `ClearParameter` hands it back. It raises
for a name the graph does not declare, once the graph has loaded; before that
the value is kept and applied when the file arrives.

`LoadAnimation` goes on working beside a graph: a track a script plays is
mixed with the graph's first layer by its weight, as two tracks always were.

Both signals are fired on the simulation's clock and deferred, like every
signal.

## In a match

**The player travels with its character** -- its `Graph` and `Retargeting`,
when the character is made and when one changes -- **and nothing of the
animation does.** No clip, no time, no state: every machine steps its own copy
of the graph from what already arrives of the character.

- A parameter with a source is read on each machine: the body's speed, whether
  it is on the ground, an attribute.
- **A server-side attack is one line**: `character:SetAttribute("Attack",
  count)` fires the trigger on every machine, because an attribute replicates
  and the trigger fires when it changes.
- Somebody who joins late sees the state the values that arrive put the graph
  in. A first sight of a value is not a change: an attack that ended before
  they joined is not played again.
- `SetParameter` is the machine's own. For something every machine must show,
  set an attribute the graph reads.

What two machines show is the same state from the same inputs, not the same
phase to the tick. On the machine with authority the graph is simulation --
the pose it makes is the one a `Bone` and a hitbox read -- and what a hit does
is decided there. Nothing in a game's rules should read a replica's graph.

A replica that predicts its own character runs some ticks again when the
server corrects it. **The graph is not stepped again**: a correction reaches
it as its inputs changing and shows as a fade, and an event fires once, not
again for a tick that was run twice.

## What it costs

A graph makes tracks, and the rest is what tracks always cost: a body seen
small is posed every second, fourth or eighth tick, and a crowd on one rig in
one state at one moment shares one pose
([Skeletal animation](manual:animation/skeletal)). Stepping a graph is a few
comparisons a layer a tick.

## In the editor

A graph is in Content with its own icon, and `Graph` in Properties offers the
project's graphs. With a player selected while the game runs, Properties shows
the graph live: each layer's state, how far through it is and what it is
fading from, and every parameter with where it comes from and a control that
sets it -- as `SetParameter` would, until the button beside it hands it back.

A graph is written in a text editor. Saved, it is read again by a running
`ludwerk dev` session, and every player on it starts from its first states.

## What is not here

- **No root motion**: a clip plays in place and the `CharacterBody` moves the
  character.
- **No graph inside a state**: a state is a clip or a blend.
- **No events for a track a script plays**: an event is a graph's.
- **No curve on a fade**: a fade is linear.
- **No drawn editor of states and arrows** yet.

## Where to look next

- [Skeletal animation](manual:animation/skeletal) -- clips, tracks and what a pose is
- [Retargeting](manual:animation/retargeting) -- one library of clips on many bodies
- [Reaching, looking and feet](manual:animation/reach-and-look) -- bending a pose to meet the world
- [Multiplayer](manual:guides/multiplayer)
- [`AnimationPlayer`](api:AnimationPlayer)
