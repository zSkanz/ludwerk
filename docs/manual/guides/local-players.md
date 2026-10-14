# Several players at one screen

Two to four people at one machine, each with a controller: each is a
`Player`, each can have a view of the window and an interface of their own,
and they can join a match together.
`examples/41-couch` is all of this page in one script.

## A player a seat

The machine's first player is `NetworkService.LocalPlayer`. Each one after
it is asked for:

```luau
--!strict
local InputService = game:GetService("InputService")
local NetworkService = game:GetService("NetworkService")

local guest = NetworkService:AddLocalPlayer()
if guest ~= nil then
    -- Which controller is this player's: what moves them, and what steps
    -- through their own interface.
    local seat = Instance.new("InputContext")
    seat.Player = guest
    seat.GamepadId = 2
    seat.Parent = InputService
    guest.GamepadId = 2
end
```

`AddLocalPlayer` answers `nil` when four are seated, and on a dedicated
server, which has nobody at it. `GetLocalPlayers` lists them in the order
they sat down; `RemoveLocalPlayer` takes one away. A player's input is read
as any player's is, with `Player:GetIntent`, so the code that moves a
character does not know whether its player is at this machine.

## A view each

A local player with a `Camera` has a part of the window, and
`Workspace.ScreenSplit` says how the window is shared:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

for _, player in NetworkService:GetLocalPlayers() do
    local camera = Instance.new("Camera")
    camera.Parent = workspace
    player.Camera = camera
end
workspace.ScreenSplit = Enum.ScreenSplit.Grid
```

| `ScreenSplit` | Two | Three | Four |
|---|---|---|---|
| `Columns` | left, right | three columns | four columns |
| `Rows` | top, bottom | three rows | four rows |
| `Grid` | left, right | three quarters, one empty | the four quarters |
| `Dynamic` | one picture, or two across a line | one picture, or the grid | one picture, or the grid |
| `None` | one picture through `CurrentCamera` | | |

Views are given in the order the players joined, to the ones that have a
camera. One player with a camera has the whole window through it. The
empty quarter of three is black, and yours: a `ScreenGui` can put a map
there.

**A view is the window's own pixels.** It is drawn at the size of its part
with everything one picture has -- its own shadows, exposure and
anti-aliasing -- and it is not a picture shown through the interface. The
world, its animation and what a room reflects are shared between the
views.

- The line between views is `Workspace.SplitLineThickness` (pixels of a
  window 1,080 high; 0 for none) in `SplitLineColor`.
- `Camera.ViewportSize` is the size of the camera's part, and
  `WorldToViewportPoint` answers in window pixels, inside that part. A
  click is a ray through the view it lands in.
- **A sequence that takes the camera takes the screen**
  ([Cinematics](manual:animation/cinematics)): a cutscene is one picture for
  everybody, and the views come back when it ends.
- Frame generation is off while `ScreenSplit` is set.

## Together, and apart

A game seen from above does not want a line down the middle while its
players stand side by side. `Enum.ScreenSplit.Dynamic` is one picture
through `Workspace.CurrentCamera` while `Workspace.SplitBlend` is 0, and the
players' own views above that. `@engine/splitscreen` drives it:

```luau
--!strict
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local splitscreen = require("@engine/splitscreen")

local screen = splitscreen.follow({ SplitDistance = 24 })
for _, player in NetworkService:GetLocalPlayers() do
    local body = player.Character
    if body ~= nil then
        screen:Add(player, body)
    end
end

RunService.Heartbeat:Connect(function(dt: number)
    screen:Update(dt)
end)
```

**With two players a line appears and turns.** One camera frames both
while they are within `SplitDistance`, pulling back as they part. Past it
the window parts across the direction between them: each player is held in
the middle of their own side, and the line turns as they walk round each
other. Nothing cuts -- at the distance the screen parts, the two pictures
are the one picture.

**The turning line is the two-player form.** A line through the middle of a
window makes two regions and no more. With three and four the module does
the right thing in the same call: one camera frames everybody while they
are together, and when they spread each has a quarter of the window, faded
in over `FadeTime`.

Choose `SplitDistance` so the one camera, at `Offset`, holds two players
that far apart whichever way they stand: that is the picture the screen
parts from.

The module is Luau and in the open. A game that wants another rule writes
the two numbers itself: `SplitBlend` (0 to 1) and `SplitAngle`, the way
from the first player to the second on the screen in degrees -- 0 with the
second to the right, 90 above. A view across a line is only lit on its own
side of it, so two views together light one window and not two.

## An interface each

```luau
--!strict
local NetworkService = game:GetService("NetworkService")
local UIService = game:GetService("UIService")

for _, player in NetworkService:GetLocalPlayers() do
    local hud = Instance.new("ScreenGui")
    hud.ReferenceHeight = 720
    hud.Player = player
    hud.Parent = UIService
end
```

A `ScreenGui` with a `Player`:

- **is laid out in that player's view** while the window is shared in
  rectangles, and scaled by the view's height -- a HUD is as large in a
  quarter of the window as the whole window's is in the window. In one
  picture, and for two views across a line, it is the whole window;
- **has a selection of its own**, `ScreenGui.SelectedObject`;
- **is stepped by that player's controller** (`Player.GamepadId`) and by no
  other. Four players each choosing a card are four of these, each moved by
  its own d-pad in the same frame, and `Activated` fires on the button that
  player pressed.

A `ScreenGui` with no `Player` is everybody's, as it always was: the whole
window, stepped by `UIService.GamepadId`. There is one pointer, one
keyboard and one touch screen, and they go where they point; the arrow
keys and Enter step the first player's interfaces as well as everybody's.

**When its player leaves**, the interface is nobody's: it is not drawn or
pressed, and `Player` reads `nil`. Destroy it with the player, or give it
to the next one to sit down.

`@engine/settings` and `@engine/subtitles` are everybody's: one settings
screen for the machine, one line of subtitles for the window.

## What is heard

Each view's camera is an ear, and **a sound is heard by the ear it is
nearest to**: as loud as it is from there, and to that listener's left or
right. Two players at two ends of a map each hear what is beside them,
once. A sound that is not positional is unchanged, and while a sequence
has the screen its camera is the one ear.

## In a match

A machine's players are players of the match it is in.

- **Hosting**, `AddLocalPlayer` is what it is offline: the guests are the
  host's own, and everybody else sees them as they see any player.
- **Joined to somebody's match**, `AddLocalPlayer` answers a `Player` at
  once and asks the host for a seat. The guest's `UserId` is a provisional
  one for a tick or two, then the number everybody knows the guest by;
  wait for it before keying anything by `UserId`. A full match refuses, and
  the guest leaves (`PlayerRemoving`).
- `FireServerFor(guest, ...)` is heard by the server from the guest, and
  `FireClient(guest, ...)` arrives at the guest's machine as
  `LocalClientReceived(guest, ...)`.
- `Host` and `Join` start a world anew, so seat the guests again in it.

**A guest's character on a machine that joined is predicted, as the first
player's is**. It moves in the tick its stick does, whatever the
link, and the server's word corrects it when the two disagree -- each
character by itself: a correction of one does not move the other. What it
asks of a game is what one predicted character asks: step each local
player's character on the client with the code the server steps it with.

```luau
RunService.Heartbeat:Connect(function()
    for _, player in NetworkService:GetLocalPlayers() do
        local body = player.Character
        if body then
            Drive.step(player, body) -- the server calls the same for every player
        end
    end
end)
```

A predicted step (`BindToPredictedStep`) runs for each of them with that
player's own intents, and what it writes on the character is answered by the
server for each.

A guest who sits down in the middle of a match is predicted from the tick
the host numbers it; a guest who gets up takes nothing of the others with it.

**Which of the two is the link treating badly.**
`NetworkService:GetStats().LocalPlayers` has an entry for each character this
machine predicts -- `UserId`, `Corrections`, `CorrectionsPerSecond`,
`LastCorrection` -- and the overlay's Network panel shows the same two rows
a player when there is more than one. Started with `--net-log-corrections`,
each correction's line in the log says whose it was.

Two people hosting have none of this: nothing is far.

## What it costs

Measured on `examples/41-couch` with a field of a thousand towers, at 1080p: two views cost 1.4 times one and four cost 2.1.
What grows with the views is what is done a view -- choosing what to draw,
and the sun's shadows -- and the pixels are shared out. Two views across a
line cost about a tenth more than two columns: the lit pass is one
window's, and what follows it is each view's whole picture.
