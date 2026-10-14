# 0205 — Several players at one screen: a view each, a line between two, an interface each, guests in a match

- Status: accepted; built (protocol 48)
- Date: 2026-10-10
- Amends: ADR 0107 (which named split screen as the next use of its views),
  ADR 0195 (which refused guests in a match and an interface a player)
- Builds on: ADR 0134 (what is drawn between ticks), ADR 0202 and 0203 (the
  environment every view reads), ADR 0204 (a view taken by a sequence), ADR
  0076 and 0163 (prediction)

## The question

ADR 0195 gave a game a second, third and fourth `Player` at one machine and
drew one picture for all of them. Four things were left to the game or
refused: a picture each, the picture that is one while they are together
and two when they part, an interface each can use at once, and bringing
somebody on the same sofa into a match on the network. What does the engine
do for each, and what stays the game's?

## What is there

Read before this was written.

- **The renderer draws a view into a target and remembers each view apart**
  (`RenderTarget::view`, ADR 0107): its depth and colour targets, the
  exposure it adapted to, its temporal history, the sun's cascades fitted to
  it. The main view is 0 and is drawn straight into the window's target; a
  `CameraTexture` is another, capped at 1,024 pixels a side, budgeted four a
  frame, and shown through a material or the interface.
- **A frame extracts one snapshot**, through the workspace's `CurrentCamera`
  or a view the renderer is told (`ViewOverride`: the editor's, a
  sequence's).
- **One environment for every view** (ADRs 0202, 0203): the sky, the probe
  pictures and the volumes' light live in one array, taken and lit once.
- **One selection and one controller navigate the interface**
  (`UIService.SelectedObject`, `UIService.GamepadId`); one pointer.
- **One listener**: `AudioSystem::update` takes one ear.
- **A peer is one player.** The `Intent` message is one player's actions a
  tick; prediction steps "this machine's own character", singular; and
  `AddLocalPlayer` answers `nil` unless the world is `Solo`.

## The decision

### 1. A view a player, in the window

- **`Player.Camera: Camera?`** -- the camera a local player looks through.
  This machine's own: it is not replicated and not hashed.
- **`Workspace.ScreenSplit: Enum.ScreenSplit`** -- `None` (what it is: one
  picture through `CurrentCamera`), `Columns`, `Rows`, `Grid`, `Dynamic`.
  Picture: not replicated, not hashed.

With `Columns`, `Rows` or `Grid`, **every local player that has a `Camera`
has a view**, in the order they joined:

| Players with a camera | `Columns` | `Rows` | `Grid` |
|---|---|---|---|
| 1 | the window | the window | the window |
| 2 | left, right | top, bottom | left, right |
| 3 | three columns | three rows | top left, top right, bottom left |
| 4 | four columns | four rows | the four quarters |

Three in a `Grid` leaves the fourth quarter black and the game's: a map, a
score, anything a `ScreenGui` puts there. Equal views are the fair default;
a game that wants one wide and two narrow sets cameras and uses `Rows`.

**Each is a view of the renderer in its own right**, drawn at the size of
its rectangle and put into the window: no cap, no budget, the main view's
quality. Not a `CameraTexture`: nothing is shown through the interface, and
the window's pixels are the view's.

**What a view has of its own**: what it draws (its own extraction, culled
to its own frustum), depth and colour, the sun's cascades fitted to what it
sees, ambient occlusion, exposure and how it adapts, temporal history and
anti-aliasing, post.

**What every view shares**: the world and its poses (animation is sampled
once), the mesh, texture and material libraries, **the environment array**
-- the sky, the probe pictures of ADR 0202 and the volumes of ADR 0203 are
taken and lit once and read by all -- the `CameraTexture` feeds (drawn once,
before any view), and the frame's own interface.

**Not with a split**: frame generation (ADR 0165) is off while
`Workspace.ScreenSplit` is set at all -- it interpolates one picture, and a
window of several is not one. While it is set and not only while more than
one view is drawn, so a screen that parts and joins does not start and stop
it. FSR 2 upscales the main view alone; a view of a shared window is
smoothed by the temporal pass with its own history, and scaled as the
settings' render scale says.

**A sequence takes the screen** (ADR 0204): while a `SequencePlayer` with
`TakesCamera` plays, the split is put aside and the window is one view
through the sequence's camera. A cutscene is one picture for everybody at
the screen. The views come back the frame it ends, each with the history
it had.

**`Camera:WorldToViewportPoint` and the pointer** answer in the view the
camera is drawn in: a point is in that view's rectangle, and a click in a
rectangle is a ray through that view's camera.

### 2. One picture while they are together, two when they part

`Enum.ScreenSplit.Dynamic`.

- **`Workspace.SplitBlend: number`**, 0 to 1. At 0 the window is one view
  through `Workspace.CurrentCamera`, and that is all that is drawn: a game
  that never parts its players pays for one view. Above 0, **with two
  players**, their views are **put together across a line through the
  window's middle**.
- **With three and four** it is the `Grid`, faded in: a line through the
  middle of a window makes two regions and no more, so the turning line is
  the two-player form. `SplitBlend` is then how much of the grid shows over
  the one picture -- both are drawn on the way, for as long as the fade
  lasts -- and at 1 the grid alone.
- **`Workspace.SplitAngle: number`**, degrees: which way the line runs. The
  first player's view is on the side the angle points away from.
- **The line** is drawn by the composite: nothing at 0, its full width at 1
  (`Workspace.SplitLineThickness`, `SplitLineColor`), so it appears and goes
  rather than cutting in.

**A view across a line is drawn on its side of it.** The line is known
before either view is drawn, so each view writes the near plane into its
depth over the side it is not seen on, before anything else: every surface,
the sky and everything blended fails its depth test there, and no material
is run for a pixel nobody sees. The two views together light one window's
pixels, not two. What still runs over each view's whole picture is what
comes after the lit pass and has no depth test to fail -- bloom, the
anti-aliasing, the tone curve, the exposure's meter -- and that is the
whole of what a line costs over two columns (the numbers are below). The
exposure is metered on the side that is seen: the other is black, and half
a picture of black read as a dark room.

**The engine's is the mask and the composite. Where the line sits and when
it appears are the game's** -- two numbers a frame -- because they follow
from where a game puts its cameras, which is the game's. And because the
common case should be a line and not a project: **`@engine/splitscreen`**,
a module that does it for a game seen from above or from the side, in one
call for two, three or four:

- **Two.** While they are within `SplitDistance` of each other one camera
  frames them both, pulling back as they part. Each player's own camera
  looks at the point half of `SplitDistance` from its player towards the
  other, and no farther than the middle between them -- so at
  `SplitDistance` the two pictures ARE the one picture, and the screen
  parts with no cut: the line appears across the direction between the
  two, each holds the middle of their own side, and it turns as they walk
  round each other.
- **Three and four.** One camera frames everybody while they are together;
  when the farthest pair is past `SplitDistance` the grid fades in over
  `FadeTime` (a third of a second), and out again a little inside that
  distance, so a group at the edge of it does not flicker.

It is Luau, in the open, and a game that wants another rule copies it.

### 3. An interface each

- **`ScreenGui.Player: Player?`** -- whose interface it is. With a player:
  it is **laid out in that player's view** while the screen is split (the
  whole window otherwise, and in `Dynamic`), it has **a selection of its
  own** (`ScreenGui.SelectedObject`), and it is **navigated by that
  player's controller** and by no other. `nil` is what an interface is
  today: everybody's, in the whole window, navigated by
  `UIService.GamepadId`.
- **`Player.GamepadId: number`** -- the controller a local player holds; 0
  is none said. Set by the game when a controller joins, as it sets an
  `InputContext`'s. It is what navigates that player's interfaces, and
  nothing else: an `InputContext` keeps its own `GamepadId`.

Four players each picking a card is four `ScreenGui`s, each with its
`Player`, each stepped by its own controller in the same frame; `Activated`
fires on the button that was pressed, which is in one player's interface.
The pointer, the keyboard and touch are one each and go where they point:
whoever holds the mouse clicks any interface. The keyboard's arrows and
Enter step the first player's interfaces as well as everybody's.

**A player's view is that player's window.** Its interface is laid out in
the view's rectangle and scaled by the view's height against
`ReferenceHeight`, so a HUD is as large in a quarter of the window as the
whole window's HUD is in the window. Two views across a line are each the
whole window: an interface has no place on one side of a line that turns.

**When its player leaves**, a `ScreenGui` with a `Player` is nobody's: it is
not laid out, drawn or pressed, and `Player` reads `nil`. It comes back when
it is given another player, or `nil` for everybody. A game destroys it with
the player, or keeps it for the next one to sit down.

**`@engine/settings` and `@engine/subtitles` are everybody's**: one settings
screen for the machine and one line of subtitles for the window, as their
interfaces have no `Player`. Any controller steps the settings.

### 4. Guests in a match

**A machine's local players are players of the match it is in.** Protocol
48.

- **On a host that plays**, `AddLocalPlayer` is what it is offline: the
  authority is this machine, its guests are its own players, and everybody
  else is told of them in the roster as of any player. Nothing is predicted
  because nothing is far.
- **On a machine that joined**, `AddLocalPlayer` answers a `Player` at once
  and asks the authority for a seat (`GuestJoin`, seats 1 to 3). The
  authority makes a `Player` as it does for a connection, gives it a number,
  and says so (`GuestSeat`); the guest's `Player` here takes that number, as
  this machine's first took its own on being welcomed -- so `UserId` is the
  provisional one for a tick or two, and then the one everybody knows the
  guest by. `RemoveLocalPlayer` gives the seat back (`GuestLeave`); a
  machine that leaves takes its guests with it; a full match answers that
  the seat is nobody's, and the guest leaves. Four players a machine.
- `Host` and `Join` no longer raise for a machine with guests. A world is
  made anew by either, so the guests of the world before are not carried:
  the game seats them again in the one it joined.
- **The machine's input is one stream.** The `Intent` message's tick holds
  every local player's intents: an entry that is no action (`0xFFFE`) says
  whose the entries after it are. One stream and not one a player, so a
  tick of a machine's input arrives whole or not at all, and the queue, its
  delay, what stands in for a late tick and when the stream is anchored
  again (protocols 22 to 40) are the machine's -- unchanged, and dealt by
  seat when a tick is applied. An entry for a seat nobody sits in is
  dropped.
- `FireServerFor(player, ...)` reaches the authority as that player's, and
  `FireClient(guest, ...)` arrives at the guest's machine as
  `LocalClientReceived(guest, ...)`, as ADR 0195 has it with no wire: two
  bits of a remote message's flags say which of the machine's players it
  is from or for.
- **What a machine is sent is what any of its players is near**: interest
  (ADR 0162) is measured from each of them, and a part any of them owns is
  the machine's.
- What a guest does NOT have: an account, a save slot or an invitation of
  its own -- it is the machine's second pair of hands.

**A guest's character is not predicted on a machine that joined -- not
yet.** It is drawn as any other player's is: from what the authority says
of it, a trip to the authority and back after the stick moved. The
machine's first player is predicted as always. This is the one thing the
decision above asked for and this does not do, and the reason is in the
code: prediction is one character's from end to end -- one owned id, one
history of predicted places, and one rollback (ADR 0133) that puts the
simulation back and steps it again with ONE stream of input when the
authority disagrees. Two predicted characters is that rollback stepping
both again with both players' input, which is its own piece of work with
the acceptance of ADR 0133 and of the smoothness work to pass again. On a
host that plays there is nothing to predict, so two at a sofa hosting a
match have none of this.

### 5. What is heard

**Each sound is heard by the listener it is nearest to.** With several
views the ears are the views' cameras; a positional sound is attenuated by
its distance to the nearest of them and placed left or right as THAT
listener hears it. Two players at two ends of a map each hear what is
beside them at full volume, once -- not twice, and not at the average of
two places where neither is. A sound that is not positional is unchanged.
While a sequence has the screen, its camera is the one ear.

## What it costs

The expectation, written before anything was measured so the measurement
could contradict it: extraction and the sun's cascades scale with the
number of views; the lit pass and post with the pixels, which are the same
pixels shared out; so two views cost less than two frames and four less
than four.

**Measured**: `examples/41-couch` with a field of 961 towers that all cast,
1920x1080, Release, an RTX 4070 Ti SUPER; a whole frame, CPU and GPU
together (`--gpu-frame-times`, which waits for the GPU once a frame); three
runs of 600 frames each, whose medians agree to a hundredth of a
millisecond.

| The window | A frame | Against one view |
|---|---|---|
| One view | 0.94 ms | 1 |
| `Columns`, two | 1.35 ms | 1.44 |
| `Rows`, two | 1.33 ms | 1.42 |
| `Grid`, three | 1.56 ms | 1.66 |
| `Grid`, four | 2.01 ms | 2.14 |
| `Dynamic`, two together (one picture) | 1.03 ms | 1.10 |
| `Dynamic`, two apart, the line upright | 1.52 ms | 1.62 |
| `Dynamic`, two apart, the line at 30 degrees | 1.51 ms | 1.61 |

And its parts, on the CPU: extraction is 0.11 ms a view (0.11, 0.21, 0.32,
0.43 for one to four); recording the sun's cascades 0.03 ms a view.

What the numbers say:

- **Two views are 1.4 frames and four are 2.1**, so the expectation held:
  what scales with the views is what is done a view, and the pixels are
  shared out.
- **A line costs 0.17 ms more than two columns** (13%), and the same at any
  angle. That is what is NOT masked: each view's bloom, anti-aliasing, tone
  curve and meter over a whole window where a column's are over half of
  one. The lit pass is masked and costs what two columns' does. Cutting the
  rest would be a scissor on every pass after the lit one, which helps only
  while the line is upright or level -- at 30 degrees a side's bounding box
  is the whole window -- and was not built for that.
- **The measure by pass (`gpu_pass_times`) does not compare these.** It
  stops the frame after every pass, and a frame of four views has four
  times the passes: in a scene this light the stops are most of what it
  reports. `--gpu-frame-times` was added for this, and is what to use.

**Four views at 2.1 times one is about twice, in this scene.** In a scene
whose cost is its casters and not its pixels it would be more, because each
view fits and draws the sun's cascades for itself. What would be lowered
first, and is not built because these numbers do not ask for it: the
cascades' resolution by the view's share of the window (a quarter of the
pixels needs half the texels a side for the same shadow on the screen), and
after that one fit over the union of views whose cameras are near each
other, which a game seen from above with everybody in one arena is.

## What is still to be decided

- **Three and four controllers on hardware.** Two are confirmed (ADR 0195);
  the gates here drive four as recordings.
- **A console.** The Developer Mode player (ADR 0190) with two to four
  controllers, split, is run by hand.
- **A match with a guest across two real machines**, on a network that is
  not this one's loopback.

## Decided while built

- **The first view is the frame's own.** A frame extracts one snapshot
  before anything else is known of it -- what the terrain is levelled
  from, where the pointer's ray goes, what the debug draw is over. With the
  window shared that snapshot is the first player's view, at the size of
  its part; the others are extracted after it, each with everything the
  first was given, built again for its own camera where it faces one (a
  ribbon, a billboard, the interface on a wall).
- **Each view is a picture of its own, in the window's format, and one pass
  makes the window of them** (`look_views.hlsl`): the views to their
  rectangles or across the line, the line drawn, and the one picture over
  them by a weight for the fade. The views keep their renderer state --
  their history -- while a sequence has the screen, and give it back when
  `ScreenSplit` is `None` again.
- **The terrain, the foliage and a block world are as fine under every
  view's camera as under the first's**: what is decided by how far the
  viewer is, is decided by the nearest of them.
- **Each view counts its own frames** for the temporal pass's sample
  positions. One count for every view gave each of four views two of the
  eight positions, by turns.
- **`Camera.ViewportSize` is the camera's part of the window**, and
  `WorldToViewportPoint`, `ViewportPointToRay` and a `ClickDetector`'s
  pointer answer in window pixels through the view the point is in.
- **`--gpu-frame-times`**: a frame waits for the GPU once, so
  `--frame-stats` says what it costs both. For comparing frames with
  different numbers of passes, which the measure by pass cannot.

## Not decided here

- **A guest's character predicted on a machine that joined** (above): the
  next piece of this. *Decided since, and built:
  ADR 0208.*
- A view a REMOTE player (spectating another machine's camera).
- More than four players a machine.
- A turning line for three or four (the regions are no longer halves).
- A pointer a player (two mice). And the pointer on an interface in the
  world -- a button on a wall -- presses it through the first view only.
- A swarm's agents (ADR 0162) are told to a machine by where its first
  player is; its guests see the ones that are near the first.
- A scissor on the passes after the lit one, for a line that is upright or
  level.
