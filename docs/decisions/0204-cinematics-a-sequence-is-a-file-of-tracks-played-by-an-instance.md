# 0204 — Cinematics: a sequence is a file of tracks, played by an instance

- Status: accepted; built (protocol 47)
- Date: 2026-10-10
- Builds on: ADR 0107 (views), ADR 0134 (what is drawn between ticks), ADR
  0197 (animation graphs), ADR 0196 (morph targets), ADR 0200 (lines, captions,
  mouths), ADR 0124 (scenes at run time), R10

## The question

A game has a moment it wants to show: two characters meet, the camera cuts
between them, one speaks, a lamp comes on. Today that is a script of
`task.wait` and tweens, which cannot be scrubbed, cannot be skipped to a
right ending, and is written by whoever can program. What does a game make
instead, who plays it, and what does every machine in a match see?

## What is there

Read before this was written.

- **Everything a sequence would drive exists, and nothing drives it by a
  clock.** A camera is an instance. An `AnimationPlayer` plays a clip
  (`LoadAnimation`, whose track has a `TimePosition` that can be set) and takes
  graph parameters. `DialogueService:Say` says a line in each machine's own
  voice and text languages, with its caption and its mouth, and a line's slot
  is as long as its longest language. A `Sound` plays and seeks. Any property
  can be written; a morph target has a weight a script sets.
- **`TweenService`** moves one property to one goal with an easing. It is the
  nearest thing to a track, and has no file, no timeline and no seek.
- **The renderer can be told which view to draw** (`ViewOverride`): the
  editor's viewport is not the game's camera, and the two do not fight over
  one transform. That is the precedent for a sequence's camera.
- **An asset that is readable JSON, hot reloaded, with a picker of its own**
  is how an animation graph and a face map are kept.
- **Nothing is a timeline**, in the engine or in the editor.

## The decision

### 1. A sequence is a file

`*.sequence.json`, under the project's content: a list of **tracks** over
seconds, and the **names** it expects to be given instances for. Readable,
diffable, hot reloaded like an animation graph, and a content kind of its own
so a picker offers sequences and nothing else.

```json
{
  "format": "sequence",
  "version": 1,
  "bindings": ["Anna", "Bruno", "Lamp"],
  "tracks": [
    { "type": "camera", "keys": [
        { "time": 0, "position": [4, 1.7, 6], "lookAt": "Anna", "fieldOfView": 50, "cut": true },
        { "time": 3, "position": [1, 1.6, 3], "lookAt": "Anna", "fieldOfView": 35, "ease": "sineInOut" },
        { "time": 3, "position": [-2, 1.7, 2], "lookAt": "Bruno", "cut": true } ] },
    { "type": "animation", "target": "Anna", "keys": [
        { "time": 0.5, "clip": "asset://anim/library.glb#Wave", "fade": 0.2 },
        { "time": 4, "parameter": "Talking", "value": true } ] },
    { "type": "line", "keys": [ { "time": 3.2, "line": "meeting.anna_1", "at": "Anna" } ] },
    { "type": "sound", "keys": [ { "time": 0, "sound": "asset://music/meeting.ogg", "category": "Music" } ] },
    { "type": "property", "target": "Lamp/PointLight", "property": "Brightness", "keys": [
        { "time": 5, "value": 0 }, { "time": 6, "value": 3, "ease": "quadOut" } ] },
    { "type": "morph", "target": "Anna/Face", "key": "Smile", "keys": [
        { "time": 6, "value": 0 }, { "time": 6.4, "value": 1 } ] },
    { "type": "event", "keys": [ { "time": 6, "name": "LampOn" } ] }
  ]
}
```

- **camera**: keys of where it stands and what it looks at -- a point, or a
  bound instance, followed as it moves -- and its field of view. A key marked
  `cut` is arrived at at once; any other is eased into from the key before,
  by the easings `TweenService` has.
- **animation**: on a bound character's `AnimationPlayer`: a clip begun at a
  time (with a fade), or a graph parameter set at a time.
- **line**: a line of the game's lines files, said from a bound instance, as
  `DialogueService:Say` says it. **Its slot on the timeline is the line's
  slot** -- the longest of its languages -- so a sequence is the same length
  in every language and nothing is cut off in the long one.
- **sound**: a sound or a piece of music begun at a time, from a bound
  instance or from nowhere.
- **property**: any property of a bound instance or of a descendant named by
  a path from it, keyed over time: numbers, vectors, colours, `CFrame`s (a
  door's turn), booleans and enums (which step). Eased between keys.
- **morph**: one shape key's weight over time, on a bound mesh.
- **event**: a name, and data, at a time: what a script hears.

A sequence's **length** is the end of its last key or slot, said by the file
(`"length"`) when it wants to be longer.

### 2. It is played by an instance

`SequencePlayer`, placed anywhere in the world:

| Property | |
|---|---|
| `Sequence` | The file. |
| `Playing` | Read. |
| `TimePosition` | Seconds. Written, it is a seek. |
| `Speed` | 1. |
| `Looped` | |
| `TakesCamera` | True: while it plays, each machine looks through its camera track. |
| `Controls` | `Keep` or `Suspend`: whether the players' own movement is held still while it plays. |
| `Length` | Read. |

`Play()`, `Pause()`, `Stop()`, `Skip()` (section 4 says what each leaves).
`Finished(skipped)`, `EventReached(name, data, skipped)`.

**Bindings are children.** A `SequenceBinding` under the player has the
binding's name as its `Name` and a `Target`. They are in the scene file, set
in the editor by picking an instance, and set from a script by parenting one
-- so a sequence made once is played on whichever two characters are there.

### 3. A character under a sequence

Every cutscene has a character in it, and two things have to be said of one.

**Its animation, against its own graph.** A character has an animation graph
that is running when the sequence begins and must be running when it ends.
While an animation track plays a clip on it:

- **the clip wins, whole**: it is a layer over the graph at full weight, so
  what is seen is the clip and nothing of the graph's pose;
- **it comes in and goes out by a fade**: the key's `fade`, a fifth of a
  second unless it says otherwise, from whatever pose the graph was giving
  into the clip's first frame; and the same out again when the clip ends, or
  the next clip's key begins, or the sequence ends -- to whatever the graph
  is giving THEN. A clip that should hold its last frame until something
  else says so is `"hold": true`;
- **the graph goes on running underneath**, fed by its parameters as ever, so
  it has not stopped anywhere it has to restart from: a character that walked
  into the cutscene standing is standing when it is given back, and one the
  sequence moved is in whatever state its speed and its ground now put it in.

A `parameter` key is the other way of doing it -- the graph does the acting,
told to by the sequence -- and is what a character that must stay in its own
state machine uses.

**Its place, when its `CFrame` is keyed.** There is no root motion: a property
track on a character's `CFrame` or `Position` is how it crosses a room. While
such a track is playing on a `CharacterBody`:

- **it is held and placed, not simulated and fought**: gravity, its walking
  and whatever its controller asks of it are not applied, and each tick it is
  put where the keys say, facing as they say;
- **it goes where the keys say**: nothing stops it, a wall included -- the
  author has said where it is. To everything else it is a body that moves by
  itself: what stands in its way and can be pushed is pushed, and a player
  cannot shove it off its mark;
- **it is left on its feet**: when the track ends, or the sequence ends or is
  skipped, it is at the last key's place, upright -- the key's turn about the
  vertical kept and any lean dropped -- set down on the ground under it if
  the ground is within a step's height, with no speed, and simulated and
  controllable again from the next tick. A last key in mid-air is a
  character that then falls.

### 4. Seek, stop and skip

- **`Stop` leaves the world as it is at that moment**: properties where the
  sequence had got them to, clips faded out, the lines and sounds it began
  ended, held characters set down, the camera given back, its time back to
  nought. It fires no event and **`Finished` does not fire**: nothing was
  finished.
- **A seek** (`TimePosition = t`) puts every track where it is at `t`:
  properties and morphs at their values, the camera at its place, clips and
  sounds at the moment of theirs that `t` falls in. **It fires no event.** It
  is what an editor's scrub does and what a game's "rewind" would.
- **A skip lands the world where the sequence would have left it.** Every
  property and morph takes its last key's value; **every event between now
  and the end is fired, in order, with `skipped` true** -- so a door a
  handler opens is open, and a handler that only makes a noise can tell;
  lines and sounds the sequence began are stopped; clips are faded out;
  held characters are set down at their last keys; the camera is given back.
  **Then `Finished` fires, with `skipped` true** -- a game waits on one signal
  for "the cutscene is over" and reads the argument if it cares how.

### 5. Which side of the hash each track is on

The sequence's **clock is the simulation's**: `TimePosition` advances with
the tick, deterministically, and is in the hash with the player's other
properties. What the tracks do with it divides in two.

| Track | Who evaluates it | Side |
|---|---|---|
| property | the authority, each tick: it writes the property | **simulation**: hashed, replicated as the property always is |
| animation | every machine that plays it: it begins the clip and sets the parameter | each machine's own pose, as animation is: neither a track nor a parameter a script sets travels (ADR 0197) |
| event | every machine that plays it, in the tick | simulation: a handler moves the world |
| line | the authority says it: the sound it makes is an instance | the sound travels; its caption and mouth are picture, each machine's languages |
| sound | the authority makes the sound | the sound travels |
| camera | each machine, each frame, between ticks | **picture** |
| morph | each machine, each frame | **picture** (the layer a mouth writes) |

So a cutscene that moves a door moves the door in the simulation, on the
authority, where a script would have; and a camera cut is nothing a replay or
a server knows.

### 6. In a match

- **The authority starts it.** A `SequencePlayer` in the replicated world
  that the server plays is played on every machine: what travels is the
  player's own state -- `Playing`, `Speed`, and the tick at which its time was
  nought -- and each machine reads the sequence's time off that and its own
  clock. Nothing of the sequence's content travels: the file is in the build
  both ends have.
- **Each machine draws its own picture**: its camera, its captions in its
  text language, its voices in its voice language, its mouths.
- **A late joiner** is given the player as it stands: it joins the sequence
  at its present time, looks through the camera where it now is, and hears
  the lines and sounds already in the world from where they have got to.
  Events before its arrival are not fired for it; the world they changed is
  the world it was sent.
- **A client plays one of its own** by making a `SequencePlayer` that is
  local to it: a first-person flourish nobody else sees. Its property tracks
  write that machine's own instances only.
- Protocol 47: `SequencePlayer` and `SequenceBinding`.

### 7. The camera and the player's input

- **The camera is taken as a view, not as a transform.** While a player with
  `TakesCamera` plays, each machine's main view is drawn through the camera
  track -- the renderer is told which view, as the editor tells it -- and
  **`Workspace.CurrentCamera` is not written**. The game's own camera script
  goes on running and goes on being ignored; when the sequence ends, the
  picture is the game's camera again, wherever its script has kept it, with
  no handing back to get wrong. `Camera:GetRenderCFrame` answers the view
  that is drawn, so a script that needs to know can.
- **Input is the game's to give up, in one property.** `Controls = Suspend`
  holds every local player's movement and actions at rest while the sequence
  plays on that machine -- what a character controller reads is nought --
  and gives them back when it ends. **Input events still reach scripts**: a
  game binds its own key to `Skip()`. `Keep` changes nothing, for a sequence
  a player walks through.

### 8. Authoring

A **Timeline panel** in the editor, open on a `SequencePlayer`:

- the tracks down the side, the keys along a ruler of seconds, a playhead;
- **scrubbing in edit mode**: dragging the playhead puts the scene where the
  sequence is at that time, with nothing playing -- as a cape is seen to hang
  in edit mode. What the scrub touched is put back when the panel lets go of
  the sequence: a preview never dirties the scene;
- **keys from where things are**: with a property track's record on, moving
  the bound instance in the viewport, or changing the property in
  Properties, sets a key at the playhead;
- **the camera looked through**: the viewport draws through the camera track
  while the panel asks it to, and a camera key is set from the viewport's own
  view ("key from view");
- lines shown at their slots' lengths, with the text of the language the
  editor is in;
- save writes the same readable JSON a person could have written, and a file
  changed outside is reloaded.

### 9. What it costs

A sequence playing is a handful of key lookups a tick and a view override a
frame. Measured before it is called built: a sequence of fifty tracks
evaluated a tick, and the frame with and without the camera taken.

## Decided while built

What the code settled that the sections above left open, or said otherwise.

- **A property is the sequence's from its track's first key to its last.**
  Before the first the track does not write it, and after the last it is
  written no more: it stays where it was left and is the game's again. A
  seek to before a track's first key leaves the property alone. So a track
  that should hold a property from the start has a key at nought -- and a
  sequence played twice puts its lamp back by itself.
- **The animation track is every machine's**, and section 5's table says so
  now. It said the authority's; but a track a script loads and a parameter a
  script sets do not travel (ADR 0197), so a clip the authority began alone
  would have been seen on the authority alone.
- **A clip is named as `LoadAnimation` names one** -- `Clip`, or
  `file#Clip` -- and is played over every layer of the graph
  (`AnimationHost::setOver`), not among them. Two on one track cross-fade by
  the later one's `fade`. A clip that reached its end and does not `hold`
  fades out from its last frame. A paused sequence holds its clips still.
- **A shape key the sequence keys is the sequence's while it plays**: a
  weight a script left held on it is let go when the sequence begins, the
  track is drawn on the frame's layer (the one a mouth writes; two on one
  key, the larger), and when the sequence ends the weight is held where the
  track left it -- by each machine, for itself, as a script's is.
- **A look target and an `at` may be a path under a binding**
  (`Anna/Bust/Face`). A camera aimed at a model's middle is aimed at its
  belt; an `Attachment` where the face is, is what it looks at.
- **A seek picks a line up part way and not a sound**: how long a line is, is
  known before it is said; how long a sound is, is the mixer's to know.
- **What travels** (protocol 47) is `Sequence`, `Playing`, `Speed`, `Looped`,
  `TakesCamera`, `Controls`, where its time was and at which of the
  authority's ticks when it last began, was held, was sought, wrapped or
  changed speed -- and three counts: its seeks, its stops and its skips.
  The counts are what tell a replica a seek (no event) from time that passed
  (events), a stop (nothing finished) from an end, and a skip (events marked,
  `Finished(true)`) from both. Nothing is sent a tick. Measured between two
  processes on one machine: a joiner that met a sequence a quarter of a
  second in was at 1.53 s when the server's was at 1.53 s, and a three-second
  sequence lasted 180 ticks on both.
- **A carried character is an anchored one** for as long as its track plays:
  the rule an anchored character already had -- not stepped, where its
  `CFrame` puts it -- and its `Anchored` is put back when it is set down. So
  what it is put into is pushed out of it, as out of any body put there; it
  does not sweep a crate along in front of it. Section 3 said "pushed", which
  promised more.
- **`Controls = Suspend`** rests the local players' intents and, before the
  simulation, whatever was asked of their characters this tick -- a
  controller that reads an `InputAction` itself is held too. It lasts from
  the sequence's beginning to its end, a pause included.
- **The enum is `Enum.SequenceControl`**, singular, as every enum is.
- **The Timeline** is a model (`app::Timeline`) with a panel drawn over it.
  The model is tested without a window: scrub, record, key from the scene and
  from a view, move and remove, save and read back, a file changed under it,
  and the scene put back. The panel is tested without one too, against an
  ImGui that draws to nowhere and a mouse the test moves: it opens on the
  selected player, the ruler scrubs the scene, a dragged key moves.
- **And then it was looked at** (amended the same day): the editor draws into a picture with no window
  (`--editor-shots`, `tests/editorshots/`), and the first pictures of this
  panel showed what a test could not -- a field's name after its field, a
  track's name running over its first key, rows four pixels apart, the
  lines' words in the system's language and not the editor's. Fixed, and
  `editor_shots_timeline` holds the panel open and scrubbed.
- **A scrub poses the characters** (amended with it): every bound character's
  animation track is posed at the playhead -- the clip at the frame the
  moment falls on, as far faded in as it would be, held or gone as its key
  says -- and a parameter track sets its value over a graph that is
  otherwise at rest. **The graph is evaluated and not stepped**: a blend a
  parameter drives is seen, a transition it would begin is not, because
  nothing moves a clock. Letting go of the sequence puts every body back as
  its file rests it (`AnimationHost::rest`), as a property is put back.
- **Measured.** Fifty property tracks, all written, cost 6.3 microseconds a
  tick (dev build, in its unit test). The example's sequence -- nine tracks,
  three of them written every tick -- is 2.8 microseconds a tick at the
  median in a Release build. And the frame with the view taken against the
  frame with it left to the game's camera, the same picture both ways
  (Release, 1920 by 1080, 900 frames, twice each): 0.5345 and 0.5341 ms
  against 0.5339 and 0.5347 ms at the median. Nothing that can be told from
  the frame's own noise: a view the renderer is told is the view it would
  have worked out.

### A replica's clock is the authority's

Section 6 said a replica "runs on from there by its own clock". It does
not any more, and the anchor is written sooner:

- **A replica's time is read off the anchor and the authority's tick as the
  replica knows it**, every tick, and not counted on by its own ticks from
  the anchor. Its own ticks are not the authority's: against a server that
  ran slow, a replica ran ahead without bound, reached the end and said so.
  The tick it reads is its running estimate of the authority's -- counted on
  between snapshots and put right by each -- so the picture moves every
  frame and is never more than a few ticks from the authority's. Never back:
  what was said is not unsaid. A looped sequence runs on by its own ticks
  and is anchored again at each turn.
- **A replica at the end waits to be told it is over.** `Finished` is said
  once, on the authority's word; the authority may yet skip, stop or seek it
  there. `Playing` on a replica is the authority's, to the end.
- **The anchor is written with the ask** -- `Play`, `Pause`, a write of
  `TimePosition` or of `Speed` -- on the machine that decides, and again by
  the host when it has done it. Written only by the host, a tick later,
  "playing" could travel a snapshot before the anchor that says from when.

Found by `netcode_cutscene` failing once on a loaded runner, and behind it:
the test waited for ever and said nothing, which a wait that gives up with a
word now does not.

## Not decided here

- **Sequences inside sequences**, and blending two that play at once.
- **A camera rig**: dolly paths as curves through space, shake, focus pulls
  keyed with depth of field. The camera track is keys.
- **Recording a play session into a sequence.**
- **Cinematic bars, fades and titles**: they are the game's interface, and an
  event is how a sequence asks for them.

## Amended 2026-10-11: what a game in production found

Each run before it was touched. Where this section and the text above
disagree, this is what is built.

1. **`Length` is known from the line that sets `Sequence`.** It read 0
   just after `Play()`. Not because the file "had not loaded" -- a file is
   read the moment it is first asked about -- but because the number was
   written by the tick, a tick later. The property asks the host now
   (`scene::SequenceFiles`). So there is no loaded signal: nothing loads
   later. A file that is not there is 0, and says so once in the log.
2. **A property track keys a service as it keys anything.** The report was
   that it could not. It could: a binding's target is an instance, and
   `Lighting` is one -- the hour and the ambient on the service, the air
   and the sky by a path under it, a post effect bound by itself; the
   conformance suite holds all of them. What it did do was nothing, in
   silence, for a property name the target has not got or a key whose value
   is not the property's kind. It says so now, once a player
   (`app.warn.sequence_property`).
3. **A camera key in somebody's frame**: `"relativeTo": "Anna"`. Its
   `position`, its `rotation` and a `lookAt` that is a place are then in
   that instance's own space, where it stands and faces this frame -- a
   scene set wherever the character happens to be. Two keys in two frames
   are mixed in the world.
4. **A hold**: a `hold` track's key, a time and a name. The sequence's time
   stops at it -- what is AT the key is done -- `Held` says its name,
   `HoldReached` fires, and it goes on at `Release()` (or `Play()`). What
   the sequence had begun goes on: a clip keeps playing, a looped one
   loops. Nothing new begins. A skip passes it; a seek leaves it. **In a
   match** it is the authority's: every machine stands at the hold -- by
   the anchor, as at a pause -- hears `HoldReached`, and goes on when the
   authority lets go. `Held` travels (protocol 49).
5. **Markers**: a `marker` track's keys, where shots begin.
   `GetMarkers()`, `PlayFrom(name)`, `SkipToNext()` and the event
   `MarkerReached`. Skipping to the next lands the world where the sequence
   would have left it there, as `Skip` does at the end -- keyed properties
   where their keys put them, the events between heard as skipped, lines
   and sounds between not said -- and plays on. A machine that does not
   decide is told as it is told of a seek.
6. **The picture's own tracks**: `fade` (a colour, and how much of the
   picture it covers), `bars` (how far in the bars of a wider picture are,
   and its shape), drawn under the game's interface by each machine; and
   `caption`, words nobody says, shown by whatever shows a line's.

## Amended 2026-10-11: what a game in production found, the second half

Five things a game asked of a sequence that the file could not say. Where
this section and the text above disagree, this is what is built.

13. **A `cast` track brings somebody who is not in the scene.**
    `{ "type": "cast", "as": "Guard", "model": "guard", "keys": [ { "time": 1 },
    { "time": 9, "show": false } ] }`. `model` is a stamp, named as
    `Instance.stamp` names one; `as` is a name the other tracks of the file
    may play on, written before the cast track or after, and is not one of
    the file's `bindings` -- nobody is expected to give it. The instance is
    made when the track's first key is reached and put in the workspace;
    the binding it is known by is a `SequenceBinding` under the player, made
    with it, so it is every machine's as any binding is. A key with
    `"show": false` takes it out of the world and keeps it; one after puts
    it back as it left.
    - **Stopped, whoever it brought goes with it**: the sequence is back
      before it brought anybody. Played again from the start, the same.
    - **At its end, and skipped to it, they are as the last key left them**:
      there, and then the game's to keep or destroy; or gone.
    - **In a match they are the authority's**, replicated as anything it
      makes is. A replica makes nobody.
    - **Under the editor's scrub they are there between their keys**, made
      for the scrub: never saved, not a row of the tree, bound by no
      instance, and gone when the scrub lets go. **In the Timeline the
      name is a binding's** (amended 2026-10-14): listed where a track is
      added, after the player's bindings, and a track on it is not marked
      unbound where the scrub is outside the cast's keys.
    - A stamp that is not there brings nobody, and says so once.

14. **A value the game says: `"$Name"`.** Where a key of a `property`,
    `morph`, `fade` or `bars` track has a value, `"$Name"` is the
    `SequencePlayer`'s **attribute** of that name, read when the key is used
    -- a number, a boolean, a string, a `Vector3`, a `Color3`, a `Vector2`.
    Attributes travel (ADR 0106), so what a server sets every machine reads:
    nothing new is on the wire. An attribute nobody set is no value, and the
    track writes nothing for that key. A string whose value really begins
    with `$` is written with two (`"$$5"` is the text `$5`), and the writer
    puts the second back (amended 2026-10-14; it could not be keyed before).

    **And a sequence written as a table: `SequencePlayer:LoadSequence(t)`**,
    in the shape the file has. It is given a name of its own
    (`memory://...`) and played as a file is. **It is that machine's own**:
    called on a server, the world moves and every client sees it move, but
    the picture tracks are in a table no client has. A scene every machine
    is to see whole is a file with variables. A table that is not a sequence
    is an error that says where, as a file's is said.

15. **The camera, as the game would have it.**
    `SequencePlayer:BindCamera(function(cframe, fieldOfView, time) ... end)`:
    asked every frame the player's sequence has the view, in the render
    phase, handed where the sequence put the camera, the field of view that
    would be drawn and the sequence's time; what it answers -- a `CFrame`,
    then a field of view, either, or `nil` -- is drawn. It answers in the
    frame: one that waits has answered nothing. One function a player; on
    the machine that binds it only. Not asked under the editor's scrub:
    the scrub is of the scene being edited, where no script of the game has
    run, so there is no function to ask (looked at again 2026-10-14, and
    kept).

    **`"keepInSight": true`** on a camera key that looks at a bound
    instance: where something that does not move stands between the camera
    and who it looks at, the camera is brought towards them along its own
    line of sight until it is twenty centimetres this side of it. One ray a
    frame against the physics world, which the frame only reads; whoever is
    looked at, what they are part of and what moves are passed. Eased in and
    out with the key that asks.

16. **A hand that is not steady, and a path that is one curve.**
    `"shake": [metres, hertz]` on a camera key, eased between keys: the
    camera is moved across and up by as much as that, and turned by the
    angle that much is seen under from a metre away. **A function of the
    sequence's time** -- the same picture on every machine, in every run,
    and under the scrub -- with how fast it goes counted along the track, so
    a shake that quickens does not jump.

    `"path": "curve"` on the camera track: its place goes through its keys
    as one curve, each key passed at the speed and heading that take the
    camera from the key before it to the key after. A cut begins another
    curve. Focus is a `property` track on a `DepthOfFieldEffect`'s
    `FocusDistance`, which already worked: the manual says so.

17. **A picture key after a line, as heard: `"afterLine": "<line>"`.** On a
    key of a camera, fade, bars or morph track: the key is at the END of
    that line as this machine hears it -- its recording in the language the
    voices are in here -- with the key's own `time` counted on from there.
    Refused on every other track: what moves the world, or is heard by
    every machine, keeps the simulation's clock, which must be every
    machine's. **The sequence is as long on every machine all the same**:
    its length counts the line's slot, the longest of its languages, with
    the key's time after that. The voices changed while the game runs, the
    picture is laid out again. In the Timeline such a key is drawn at its
    own time, and says which line it follows.

**Item 18** of the same list -- reflections and bounce in screen space --
is not here: it is a renderer's feature of its own size, and opens the
editor's category.

**In the editor.** The Timeline adds a cast track (a name, a stamp) and keys
it there or gone; a key's tooltip shows a variable and the line it follows.
And the list of kinds a track can be added as is every kind there is: it
stopped at the seventh when there came to be twelve.

**What it is measured by.** `engine_app_tests` and `engine_asset_tests`
(eleven cases named "ADR 0204" of this batch), four cases of the
conformance suite, and `examples/43-ambush`, run to its end by
`example_ambush_plays`: a guard a cast track brought is there when the blow
lands and gone when the file is over; the flash is what the game said the
blow was; a camera whose key is eight metres off behind a wall is 3.4 from the
hero; the function bound to the camera is asked every frame; and a sequence
the script wrote as a table plays after the file's.
