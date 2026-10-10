# Skeletal animation

An `AnimationPlayer` plays a skinned mesh's clips.

```luau
--!strict
local player = Instance.new("AnimationPlayer")
player.Parent = character      -- the Model the character is, or its one MeshPart

local walk = player:LoadAnimation("Walk")
walk.Looped = true
walk:Play(0.2)                 -- fade the weight in over 0.2 s
```

The clips come from a mesh's file: a glTF's animations, or an FBX's or a
Collada file's takes ([Meshes and models](manual:world/meshes)).

**Parent it to the `Model` whose character it is, and it drives every skinned
`MeshPart` under that model.** A character is a body, a shirt and a pair of
trousers -- several meshes wearing one skeleton -- and one clip has to move all
of them. Only one of the pieces needs to carry the clips: they are taken from
the first that does, in tree order, and the pieces are matched joint to joint
by name. Parented straight to a `MeshPart` it drives that mesh and no other,
which is what a character made of one piece wants.

A track is a handle a script holds rather than a child in the tree, and what a
script plays is that machine's own. **To have a character animate itself** --
which clips, how one becomes another, on every machine in a match -- give the
player a `Graph` ([Animation graphs](manual:animation/graphs)).

## Naming a clip

`AnimationPlayer.LoadAnimation` accepts three spellings, and the common case is
the first:

```luau
player:LoadAnimation("Walk")                           -- a clip in this player's own mesh
player:LoadAnimation("#Walk")                          -- the same thing
player:LoadAnimation("asset://clips/humanoid.glb#Walk") -- a clip in another file
```

Everything after a `#` is the clip's **name inside the file**. A path before
the `#` names the file the clip is in, **and it need not be the one this
player's skeleton came from**: one walk made once and played by every
character is the reason a clip is addressable at all. The file is read because
the track names it -- no mesh has to wear it -- and the clip is carried onto
this character's own skeleton, by what each joint is where the two are bodies
of different build and by joint name otherwise
([Retargeting](manual:animation/retargeting)).

**Load a track once and keep it.** `LoadAnimation` always returns a track, even
for a clip that is not there — a mesh that has not finished loading would
otherwise make an ordinary frame a nil index.

The readiness check is `AnimationTrack.Length`, which is zero for a clip that
was not found:

```luau
--!strict
local RunService = game:GetService("RunService")

local started = false
RunService.Heartbeat:Connect(function()
    if started then
        return
    end
    local track = player:LoadAnimation("Bend")
    if track.Length > 0 then
        track.Looped = true
        track:Play(0.2)
        started = true
    end
end)
```

## The track

`AnimationTrack` is a handle, like `Tween` — nothing parents one and nothing
finds one in the tree. It is also **the one mutable datatype**, and for a
reason: `track.Looped = true` names the one track every holder of that handle
can see, where writing to a `CFrame` would write to a copy you are about to
drop.

| Member | Access | Notes |
|---|---|---|
| `AnimationTrack.Playing` | read-only | Whether the clock is advancing. |
| `AnimationTrack.Looped` | read/write | A looped track never fires `Ended`. |
| `AnimationTrack.Speed` | read/write | 2 is twice the speed and half the duration; **0 holds the pose**. |
| `AnimationTrack.Weight` | read/write | How much it contributes when several drive one skeleton. |
| `AnimationTrack.Length` | read-only | Seconds, or zero for a clip that was not found. |
| `AnimationTrack.TimePosition` | read-only | Where the clock is. |
| `AnimationTrack.Ended` | read-only | A property holding a signal. |

`Speed = 0` is the honest way to freeze a frame. `TimePosition` is read-only in
this release: seeking a blended pose is a different feature from playing one,
and a writable seek that ignored the blend would be the wrong half of it.

## Play and Stop

`AnimationTrack.Play` starts the clip **from the beginning, every time** — the
opposite of `Tween.Play`, and deliberately: a jump animation triggered twice
should play twice.

Its optional `fadeTime` fades the weight in from zero over that many seconds,
which is what makes a walk blend into an idle rather than snapping. It fades in
**to this track's own `Weight`**, which is why one argument is enough.

**`Weight` is what you set, and a fade does not change it.** `Stop` fades the
track out and `Play` fades it back in, and after either `Weight` reads what it
read before -- so an idle played, stopped and played again plays at the weight
it had. A `Play` that lands while a `Stop` is still fading out ends the
fade-out and carries on from where the fade was, with no pop.

`AnimationTrack.Stop` ends the track, optionally fading its weight out first.
**`Ended` does not fire either way** — it is a past-tense fact about reaching the
end, and code that chains the next animation on it must not be fooled by one
that was interrupted.

A track that reaches the end of a non-looped clip **holds its last frame** until
it is stopped or replayed. `Ended` is deferred, and a character that returned to
its rest pose for the one tick before the handler ran would be a visible pop.

## Blending

Set `Weight` on several tracks and they blend. Assigning it is **immediate** —
the faded form is `Play`'s and `Stop`'s argument, and a property that took a
fade time would not be a property. Set on a track that is stopped or fading
out, it is the weight the next `Play` fades in to, and does not start it.

**Blending is a weighted average per joint and per component.** A joint no clip
drives keeps its rest transform rather than being dragged towards the origin by
tracks that say nothing about it.

The blend order is the order tracks were loaded, which is stated rather than
left to a container: two tracks at weight 0.5 have to blend the same way on
every run.

Two `AnimationPlayer`s under one mesh blend into one pose.

## The clock

Sampling happens at `RunService.PreAnimation`, on the simulation clock, so a
clip's position at a given tick is the same in a replay as it was live. A looped
track wraps keeping the fraction of a tick it overshot by, so a loop does not
drift against everything else in the scene.

## As often as it is seen

A crowd of characters is posed as often as each is seen, not every tick. A rig
neither the camera nor a shadow reaches last frame is not posed at all, and one
small on the screen is posed every second, fourth or eighth tick, the ticks
spread across the crowd. Its clips keep time either way, so it is where it
should be the moment it is seen, and a joint a script or a `Bone` asks about is
posed when asked. A world with nobody looking -- a server, a replay -- poses
every tick.

`AnimationPlayer.CullingMode = Enum.AnimationCullingMode.AlwaysAnimate` poses
its meshes every tick, seen or not: the hero, or a character whose pose a
script reads from a distance.

**How small is small is a setting**, `GraphicsService.AnimationDetail`
(`[graphics] animation_detail` in `project.toml`), a scale on the size a rig is
taken to be. At 1 a rig is posed every tick while it covers 12% of the
picture's height or more, every second tick down to 6%, every fourth down to
3%, and every eighth below that. A camera that stands well back from a crowd
sees every rig small -- from 8.5 metres behind a hero at a field of view of
70, a character 1.5 metres tall is posed every tick only within about ten
metres of the camera -- and the crowd moves in steps. Such a game raises it:

```toml
[graphics]
animation_detail = 4   # every tick down to 3%, every second down to 1.5%
```

It costs the poses it asks for, so it is a setting a player's menu may lower
on a slow machine as it lowers shadows.

A crowd of one skinned mesh is also drawn in one call a pass, each copy posed
by its own palette, and a skinned mesh has levels of detail as a static one
does.

## When a model is exported again

While `ludwerk dev` runs, a model saved again is read again: its mesh, and
with it its joints and its clips. A track finds its clip again **by name** --
an exporter is free to put the clips in another order -- and what was playing
goes on from the time it was at. A track whose clip is no longer in the file
plays nothing and still answers.

## What is not here

A track is clip playback and linear blending. States, layers that mask or
add, and blends along a parameter are an
[animation graph](manual:animation/graphs)'s; a limb that reaches, a head
that looks and feet on the ground are
[`IKControl` and `FootPlacement`](manual:animation/reach-and-look)'s. There is
no root motion -- a clip plays in place and the body moves the character --
and `TimePosition` cannot be written.

## Where to look next

- [Animation graphs](manual:animation/graphs) — a character that mixes its own clips
- [Retargeting](manual:animation/retargeting) — one clip on bodies of other proportions
- [Reaching, looking and feet](manual:animation/reach-and-look) — bending a pose to meet the world
- [Tweens](manual:animation/tweens) — for animating a property rather than a rig
- [Capes, tails and hair](manual:animation/secondary-motion) — what the body moves and no clip can
- [Faces and shape keys](manual:animation/morph-targets) — the same mesh in another shape
- [Meshes and models](manual:world/meshes)
- [`AnimationPlayer`](api:AnimationPlayer) · [`AnimationTrack`](api:AnimationTrack)
