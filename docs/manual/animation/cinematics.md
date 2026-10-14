# Cinematics: a sequence, and what plays it

A game has a moment it wants to show: two people meet, the camera cuts
between them, one speaks, a lamp comes up, a door opens. That moment is a
**sequence** -- a file of tracks over seconds -- and a `SequencePlayer` plays
it. It can be paused, sought, stopped and skipped; it is the same on every
machine of a match; and the editor's Timeline panel is the same file with a
playhead.

`examples/40-cinematic` is all of it in one room, `examples/42-nightfall`
has the hold, the shots, the fade, the bars and a camera at a walker's
shoulder, and `examples/43-ambush` has what the game tells a sequence as it
runs: who is brought, how hard the blow is, where the camera may not be.

## The file

`content/sequences/meeting.sequence.json`, written to be read:

```json
{
  "format": "sequence",
  "version": 1,
  "bindings": ["Anna", "Bruno", "Lamp", "Door"],
  "length": 16.5,
  "tracks": [
    { "type": "camera", "keys": [
        { "time": 0, "position": [0, 2.3, 6.4], "lookAt": [0, 1.6, 0], "fieldOfView": 46, "cut": true },
        { "time": 2.4, "position": [-0.3, 1.9, 2.3], "lookAt": "Anna/Bust/Face", "fieldOfView": 30, "ease": "sineInOut" },
        { "time": 5.9, "position": [0.3, 1.9, 2.3], "lookAt": "Bruno/Bust/Face", "cut": true } ] },
    { "type": "line", "keys": [
        { "time": 2.6, "line": "meeting.anna_1", "at": "Anna" } ] },
    { "type": "animation", "target": "Bruno", "keys": [
        { "time": 5.6, "clip": "Idle", "fade": 0.3, "speed": 2 } ] },
    { "type": "morph", "target": "Anna/Bust", "key": "Blink", "keys": [
        { "time": 1.9, "value": 0 }, { "time": 2.02, "value": 1 }, { "time": 2.16, "value": 0 } ] },
    { "type": "property", "target": "Lamp/Light", "property": "Brightness", "keys": [
        { "time": 0, "value": 0.5 }, { "time": 10, "value": 0.5 }, { "time": 11.8, "value": 4, "ease": "quadOut" } ] },
    { "type": "property", "target": "Door", "property": "CFrame", "keys": [
        { "time": 0, "value": [0, 1.1, -3, 0, 0, 0] },
        { "time": 12.9, "value": [0, 1.1, -3, 0, 0, 0] },
        { "time": 14.5, "value": [-0.704, 1.1, -3.591, 0, 100, 0], "ease": "quadOut" } ] },
    { "type": "event", "keys": [
        { "time": 14.5, "name": "DoorOpen", "data": 100 } ] }
  ]
}
```

`bindings` are the names the file expects to be given instances for. A track's
`target` is one of them, or a path of children's names under one:
`Lamp/Light`.

Every key has a `time`. A key marked `"cut": true` is arrived at at once; any
other is eased into from the key before it -- `"ease"` is `linear` (what it is
when not said) or a style and a direction as one word, the ones `TweenService`
has: `sineInOut`, `quadOut`, `backIn`, `bounceOut`.

### The tracks

| Type | A key says | What it does |
|---|---|---|
| `camera` | `position`; then `lookAt` (a place, or a bound instance) or `rotation` (degrees about x, y, z); `fieldOfView`; `relativeTo` (a bound instance) | Where the view is taken from. One a sequence. |
| `animation` | `clip`, with `fade`, `speed`, `hold` -- or `parameter` and `value` | Begins a clip on the target's `AnimationPlayer`, or sets a parameter of its graph. |
| `line` | `line`, and `at` | Says a line of the game's lines files, as `DialogueService:Say` does. |
| `sound` | `sound`, and `at`, `category`, `volume` | Plays a sound or a piece of music. |
| `property` | `value` | Writes the track's `property` on its target, eased between keys. |
| `morph` | `value`, 0 to 1 | One shape key's weight (the track's `key`) on a mesh. |
| `event` | `name`, and `data` | Raises `EventReached` on the player. |
| `marker` | `name` | A named point: where a shot begins. Played from, and skipped to. |
| `hold` | `name` | The sequence stops here until a script calls `Release`. |
| `fade` | `value`, 0 to 1; the track has a `color` | How much of the picture one color covers: black unless the track says. |
| `bars` | `value`, 0 to 1; the track has an `aspect` | How far in the bars of a wider picture are: 2.39 to 1 unless the track says. |
| `caption` | `caption`, and `speaker`, `seconds` | Words on the screen that nobody says, each by its key in the game's catalog. |

**A camera looks at an instance where it is drawn**, so a camera that moves
while it looks at a character keeps the character in the middle of the
picture. To aim at a face and not at the middle of a body, put an `Attachment`
where the face is and look at that: `"lookAt": "Anna/Bust/Face"`. A key that
names no field of view keeps the one before it.

**A camera key in somebody's frame**: `"relativeTo": "Anna"` puts the key's
`position`, its `rotation` and a `lookAt` that is a place in that instance's
own space -- where it stands and faces *this frame*. Two meters behind a
character and one up, whichever way the character turned and wherever the
scene happens to be played:

```json
{ "type": "camera", "keys": [
    { "time": 0, "position": [0.6, 1.7, 2.2], "relativeTo": "Anna", "lookAt": "Bruno", "cut": true },
    { "time": 3, "position": [0.6, 1.7, 1.4], "relativeTo": "Anna", "lookAt": "Bruno" } ] }
```

A move between two keys in two different frames is a move in the world: each
key's place is worked out first, and the two are mixed there.

**A track keys a service as it keys anything.** A binding's `Target` is an
instance, and `Lighting` is one. Bind the service and key it; what stands
under it is a path, as under anything:

```luau
bind("Lighting", game:GetService("Lighting"))
bind("Grade", colorCorrection)
```

```json
{ "type": "property", "target": "Lighting", "property": "ClockTime", "keys": [
    { "time": 0, "value": 17 }, { "time": 20, "value": 19.5 } ] },
{ "type": "property", "target": "Lighting/Atmosphere", "property": "Density", "keys": [
    { "time": 0, "value": 0.2 }, { "time": 20, "value": 0.45 } ] },
{ "type": "property", "target": "Grade", "property": "TintColor", "keys": [
    { "time": 0, "value": [1, 1, 1] }, { "time": 20, "value": [1, 0.8, 0.6] } ] }
```

A depth of field's `FocusDistance` is keyed the same way, which is how focus
is pulled. **A track that can write nothing says so once in the log** -- a
property name its target has not got, or a key whose value is not the
property's kind: a color is three numbers from 0 to 1, not one.

**A property's value** is a number, `true` or `false`, a word (an enum's item
by name, a string), or a list of numbers -- three for a `Vector3` or a
`Color3`, two for a `Vector2`, four for a `UDim2`. A `CFrame` is three numbers,
its place alone with its turn left as it is, or six: its place and its turn in
degrees. A track's keys are all of one kind.

**A property is the sequence's from its track's first key to its last.**
Before the first key the sequence does not touch it; after the last it stays
where it was left, and is the game's to write again. A track that should hold
a property from the start has a key at nought -- which is also what puts a
lamp back when the sequence is played a second time.

**A clip is named as `LoadAnimation` names one**: `Wave` is the mesh's own
clip, `asset://anim/library.glb#Wave` one from another file. It is played
*over* whatever the character's own animation is doing -- its graph goes on
running underneath -- fading in over `fade` seconds (a fifth of a second
unless it says) and out the same way when it ends, when the track's next clip
begins or when the sequence ends. `"hold": true` keeps its last frame until
then.

**A line's slot is as long as its longest language**, so a sequence is the
same length whatever anybody hears, and nothing is cut off in the long one.
`Length` includes it.

## The player

Put a `SequencePlayer` anywhere, name the file in `Sequence`, and give it a
`SequenceBinding` child for each name the file expects: the binding's `Name`
is the name, and its `Target` the instance. In the editor that is a pick; from
a script it is three lines, so a sequence made once plays on whichever two
characters are there.

```luau
--!strict
local meeting = Instance.new("SequencePlayer")
meeting.Sequence = "asset://sequences/meeting.sequence.json" :: Content

local function bind(name: string, target: Instance)
    local binding = Instance.new("SequenceBinding")
    binding.Name = name
    binding.Target = target
    binding.Parent = meeting
end
bind("Anna", workspace:WaitForChild("Anna") :: Instance)
bind("Lamp", workspace:WaitForChild("Lamp") :: Instance)
meeting.Parent = workspace

meeting.EventReached:Connect(function(name: string, data: any, skipped: boolean)
    if name == "DoorOpen" then
        print("the door is open", data, skipped)
    end
end)
meeting.Finished:Connect(function(skipped: boolean)
    print("over", skipped)
end)
meeting:Play()
```

| Property | |
|---|---|
| `Sequence` | The file. |
| `Playing` | Read: whether its time is advancing. |
| `TimePosition` | Seconds. Written, it is a seek. |
| `Speed` | 1. Lines and sounds are heard at their own speed. |
| `Looped` | Starts again at its end, and never fires `Finished` by itself. |
| `TakesCamera` | On: each machine looks through the camera track while it plays. |
| `Controls` | `Keep`, or `Suspend`: the local players are held at rest while it plays. |
| `Length` | Read: the end of its last key or of its last line's slot. Known from the line that sets `Sequence`. |
| `Held` | Read: the name of the hold it is waiting at, or an empty string. |

### Pause, seek, stop and skip

| | The world | Events | `Finished` |
|---|---|---|---|
| `Pause()` | as it is; clips held still; a line being said goes on to its end | none | no |
| `TimePosition = t` | every track put where it is at `t` | **none** | no |
| `Stop()` | **as it is at that moment**; what it began ended; its time back to nought | none | **no** |
| `Skip()` | **where the sequence would have left it**: every property and shape key at its last key, what it began ended | every one not yet reached, in order, with `skipped` true | yes, with `skipped` true |
| reaching the end | where its keys left it | each once, in the tick its time came | yes, with `skipped` false |

A game waits on `Finished` for "the cutscene is over" and reads its argument
if it cares how. A handler of `EventReached` that opens a door opens it
whether or not it was skipped; one that only makes a noise checks `skipped`.

### A hold

A `hold` track's key stops the sequence where it stands: a choice to be made,
a button to be pressed. Time goes as far as the key -- what is *at* the key is
done -- `Held` reads its name, and `HoldReached` fires.

```luau
meeting.HoldReached:Connect(function(name: string)
    if name == "Answer" then
        local choice = askThePlayer()
        meeting:Release()
    end
end)
```

`Release()` goes on from there, and so does `Play()`. **What the sequence had
begun goes on while it waits**: a clip keeps playing and a looped one loops,
so a character goes on breathing; nothing new begins. `Skip` passes a hold; a
write of `TimePosition` leaves it.

### Shots

A `marker` track's keys are named points: where each shot begins.

| | |
|---|---|
| `GetMarkers()` | `{ { Name, Time } }`, in order of time. |
| `PlayFrom(name)` | Puts its time at the marker, as a write of `TimePosition` does, and plays. False where there is none of that name. |
| `SkipToNext()` | Skips to the next marker after where it is, and plays on. False where none comes after. |
| `MarkerReached` | Fires with the name as each is reached. |

`SkipToNext` leaves the world where the sequence would have left it at that
moment -- as `Skip` does at the end: keyed properties where their keys put
them, the events between heard with `skipped` true, the lines and sounds
between not said. So "skip this shot" is one call, and a door a skipped event
would have opened is open.

### Fades, bars and captions

Three tracks draw over the picture, **under the game's own interface**: `bars`,
then `fade`. A caption is read over a picture gone black.

```json
{ "type": "fade", "keys": [
    { "time": 0, "value": 1 }, { "time": 1.5, "value": 0, "ease": "sineOut" },
    { "time": 28, "value": 0 }, { "time": 30, "value": 1 } ] },
{ "type": "bars", "keys": [ { "time": 0, "value": 0 }, { "time": 1, "value": 1, "ease": "sineInOut" } ] },
{ "type": "caption", "keys": [
    { "time": 2, "caption": "story.three_years_later", "seconds": 3 } ] }
```

A fade's color is the track's (`"color": [1, 1, 1]` for white). The bars
leave a picture of the track's `aspect` and are as tall as that takes in the
window there is: none at all in a window already that wide. A caption is
shown by whatever shows a spoken line's -- `@engine/subtitles`, or the game's
own -- for `seconds`, or for as long as it takes to read.

### Somebody the sequence brings

A scene needs a guard who is not in the level. A `cast` track makes him:

```json
{ "type": "cast", "as": "Guard", "model": "guard", "keys": [
    { "time": 1 }, { "time": 9, "show": false } ] },
{ "type": "property", "target": "Guard", "property": "Position", "keys": [
    { "time": 1, "value": [-9, 1, -6] }, { "time": 4, "value": [-1.6, 1, -0.4] } ] }
```

`model` is a stamp, named as `Instance.stamp` names one. `as` is a name the
other tracks play on -- a property, a clip, a camera's `lookAt` -- written
before the cast track or after. It is **not** one of the file's `bindings`:
nobody gives it, the track does. He is made when the track's first key is
reached, put in the workspace, and bound under the player as `Guard`, so a
script finds him the way it finds any binding: `player:FindFirstChild("Guard")`.

- A key with `"show": false` takes him out of the world and keeps him; a
  later key puts him back as he left.
- **Stopped, he goes with the sequence.** At its end, or skipped to it, he is
  as the last key left him: there -- and then the game's to keep or destroy
  -- or gone.
- In a match he is the server's, and every machine sees him. Under the
  Timeline's scrub he is there between his keys, and nothing of him is saved.
- In the Timeline his name is in the list of bindings a track is added on,
  after the player's own, and a track on him is not marked unbound where the
  scrub is outside his keys.

### What the game says

A file cannot know how hard the blow was. Where a key of a `property`,
`morph`, `fade` or `bars` track has a value, write `"$Name"`:

```json
{ "type": "property", "target": "Hero/Flash", "property": "Brightness", "keys": [
    { "time": 6.9, "value": 0 }, { "time": 7, "value": "$Blow" }, { "time": 7.8, "value": 0 } ] }
```

```luau
ambush:SetAttribute("Blow", damage)
ambush:Play()
```

It is the `SequencePlayer`'s **attribute** of that name, read when the key is
used: a number, a boolean, a string, a `Vector3`, a `Color3` or a `Vector2`.
Attributes travel, so what the server sets every machine reads. One nobody
set is no value, and the track writes nothing for that key. A string that
really begins with `$` is written with two: `"$$5 a ticket"` is the text
`$5 a ticket`.

Where it is the *shape* of the scene that is the game's -- which shots, how
many, how long -- write the whole sequence as a table:

```luau
player:LoadSequence({
    format = "sequence",
    version = 1,
    bindings = { "Hero" },
    tracks = {
        { type = "camera", keys = {
            { time = 0, position = { 0.3, 2.6, 1.2 }, relativeTo = "Hero", lookAt = "Hero", cut = true },
        } },
        { type = "event", keys = { { time = long, name = "Down" } } },
    },
})
player:Play()
```

The shape is the file's, a place is three numbers, and what is wrong with a
table is said as it is said of a file, with where. **It is that machine's
own**: called on a server it moves the world, and every client sees the world
move -- but its camera, fades, bars and shape keys are in a table no client
has. A scene everybody is to see whole is a file with variables.

### After a line, as heard

A line is two seconds in one language and three in another, and the cut that
follows it should come when it is over. A key of a camera, fade, bars or
morph track can say so:

```json
{ "time": 0.2, "afterLine": "ambush.halt", "position": [-0.4, 0.7, 2.2], "relativeTo": "Hero",
  "lookAt": "Guard", "cut": true }
```

The key is at the **end of that line as this machine hears it**, with its own
`time` counted on from there. Only what is picture may: an event, a property
or another line keeps the simulation's clock, which is every machine's, and
the file is refused where one tries. The sequence is as long on every machine
all the same -- its length counts the line's slot, the longest language's.

### A camera that is held

```json
{ "type": "camera", "path": "curve", "keys": [
    { "time": 0, "position": [-3, 2.2, 7], "lookAt": "Hero", "cut": true },
    { "time": 3, "position": [5, 2.4, 5], "lookAt": "Hero" },
    { "time": 5.6, "position": [8, 2.6, -1], "lookAt": "Hero", "keepInSight": true },
    { "time": 7, "position": [8, 2.6, -1], "lookAt": "Hero", "shake": [0.06, 9] } ] }
```

- **`"path": "curve"`**, on the track: its place goes through its keys as one
  curve and not a line from each to the next -- no corner at a key. A cut
  begins another curve.
- **`"keepInSight": true`**, on a key that looks at a bound instance: where a
  wall or a pillar stands between the camera and who it looks at, the camera
  is brought toward them until it is this side of it. What moves is not in
  the way, and neither is whoever is looked at. One ray a frame.
- **`"shake": [meters, hertz]`**: how far the camera is shaken and how many
  times a second, eased between keys like its place. Two centimeters is a
  held breath; ten is the ground moving. It is the same shake on every
  machine and every time the sequence is played.
- **Focus** is a `property` track on a `DepthOfFieldEffect`'s `FocusDistance`.

### The last word on the camera

What no file can measure -- how near the two fighters are, which way the
player was looking -- a script decides, a frame at a time:

```luau
player:BindCamera(function(cframe: CFrame, fieldOfView: number, time: number)
    local apart = vector.magnitude(guard.Position - hero.Position)
    return cframe, fieldOfView - math.clamp(8 - apart, 0, 6)
end)
```

It is handed where the sequence put the camera, the field of view that would
be drawn and the sequence's time, **every frame the sequence has the view**,
and what it answers is drawn: a `CFrame`, then a field of view, either, or
`nil` to leave both. It runs in the render phase, so `GetRenderCFrame` answers
where things are drawn. It answers in the frame -- one that waits has
answered nothing. One function a player; `UnbindCamera` lets go of it; and it
is the machine's own that binds it, as the camera is.

### The camera is a view

While a player with `TakesCamera` plays, the picture is drawn through its
camera track and **`workspace.CurrentCamera` is not written**. The game's own
camera script goes on running and goes on being ignored, and when the sequence
ends the picture is the game's camera again, wherever its script kept it:
there is nothing to hand back and nothing to get wrong. What is heard is heard
from the same place. `Camera:GetRenderCFrame`, asked in a render phase,
answers the view that is drawn.

Where two play at once, the one that began last has the view.

### The players' controls

`Controls = Suspend` holds this machine's players at rest while the sequence
plays here: their intents read as nothing pressed -- what a script reads and
what is sent to a server -- and their characters are not walked or made to
jump, whatever a controller asked. **Input events still arrive**, so a game
binds its own key to `Skip`:

```luau
--!strict
local meeting = workspace:WaitForChild("Meeting") :: SequencePlayer
local menu = workspace:WaitForChild("Menu") :: Instance
local skip = menu:WaitForChild("Skip") :: InputAction
skip.Pressed:Connect(function()
    if meeting.Playing then
        meeting:Skip()
    end
end)
```

### A character in a sequence

There is no root motion: a character crosses a room by a `property` track on
its `CFrame` or `Position`. While that track plays on a `CharacterBody` the
character is **carried**: held where the keys say, facing as they say, and not
simulated -- neither gravity nor its controller moves it. Nothing stops it, a
wall included; what it is put into is pushed out of it, as out of any body put
there. When the track runs out, or the sequence is stopped or skipped, it is
set down: at the last key's place, upright, with no speed, and simulated and
controllable again from the next tick, so the ground under it is where it
stands a moment later.

## In a match

A `SequencePlayer` in the replicated world that the server plays is played on
every machine. What travels is that it is playing and from when; nothing of
the file does, and nothing travels a tick. Each machine reads the file its own
build has and runs the sequence by its own clock.

- **What moves the world is the authority's**: a keyed property, a line, a
  sound. They reach the others as the world always does -- the lamp as the
  lamp, the line as its sound, heard in each player's own voice language.
- **What is picture is each machine's own**: the camera, the shape keys, the
  captions, the mouths, and the clips a sequence plays.
- **Events fire on every machine**, in its own tick.
- **A machine that joins late** meets the sequence where it has got to. Events
  from before it joined are not fired for it; the world they changed is the
  world it was sent.
- **One a client makes** is its own: nobody else sees it, and its property
  tracks write that machine's instances alone.
- **A hold is the server's**: every machine stands at it, hears `HoldReached`
  and reads `Held`, and all go on together when the server calls `Release`. A
  choice one player makes reaches the server as anything a player does -- a
  remote -- and the server lets go.
- **`SkipToNext` and `PlayFrom` are the server's too.** The others are put at
  the place as they are for a write of `TimePosition`.
- **A fade, the bars and a camera key's frame are picture**: each machine's
  own, of the place its own copy of the sequence has got to.
- **Whoever a `cast` track brings is the server's**, and reaches the others
  as anything the server makes does -- with the binding the tracks know
  them by.
- **A `"$Name"` is an attribute**, and attributes travel: set on the server,
  it is every machine's.
- **A key written `afterLine` is where each machine's own voices put it**;
  the sequence is as long on all of them.
- **`BindCamera` and a sequence from `LoadSequence` are the machine's own**
  that called them.

## The Timeline panel

*View > Timeline*, with a `SequencePlayer` selected. A player that names a file
that does not exist yet opens on an empty sequence, and the first save writes
it.

- **The ruler** is seconds. Press or drag on it and the playhead goes there --
  and **the scene is shown where the sequence is at that moment**, with nothing
  playing: properties at their values, the view through the camera track,
  and **every bound character posed** -- the clip its animation track has it
  in, at that frame. What the scrub shows it puts back: saving the scene,
  starting the game or closing the panel finds the scene as it was, and the
  characters as their files rest them.
- **Run** takes the playhead through the sequence.
- **A track's `+`** sets a key at the playhead: a property's from what the
  property is now; a camera's from your view; a name -- a line, a sound, a
  clip, an event -- or a weight from a small prompt.
- **A property track's checkbox records**: while it is on, moving the instance
  in the viewport or changing the property in Properties sets a key at the
  playhead.
- **Look through the camera** draws the viewport through the camera track.
  Turn it off to fly somewhere, then **Key camera from view**.
- **A key** is dragged along its lane to move it in time; a right click
  deletes it, makes it a cut, or picks its easing. A line is drawn as long as
  it takes to say, with its words.
- **Save** writes the same file a person would have written: a track a
  paragraph, a key a line.

While a sequence is open, a property it keys shows the sequence's value at the
playhead. To change the scene's own value of such a property, close the
Timeline first -- or record, if what is wanted is a key.

**A scrub evaluates a character's animation graph and does not step it**: a
blend a keyed parameter drives is shown at the playhead, and a transition
that parameter would begin is seen only by playing, because a scrub moves no
clock.

A file changed outside the editor is read again, and so is one changed while
the game runs.

## What it costs, and what it does not do

A sequence playing is a few key lookups a tick and a view a frame: fifty
property tracks all being written cost about six microseconds a tick.

A key kept in sight is one ray a frame. A shake and a curve are arithmetic.
A function bound to the camera costs what it does.

Not there: a sequence inside a sequence, or two blended; recording a play
session into a sequence; picking up a `sound` track part way after a seek (a
line is picked up; music starts at its key).
