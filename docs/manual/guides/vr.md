# VR

A game drawn in a headset, through OpenXR: on a PC with a headset its
runtime drives (SteamVR, a Quest through Link), each eye drawn at the
headset's rate. Without a headset the same game is drawn on the screen.

`examples/48-xr-room` puts all of this page in one room: things to pick up
and throw, a teleport and a turn, and a panel of buttons.

## Turning it on

In `project.toml`:

```toml
[xr]
enabled = true     # try a headset at start; the screen when there is none
# required = true  # a game played only in a headset: without one it says so and does not start
# mirror = false   # the window stays black instead of showing the left eye
# depth = true     # the headset is given each eye's depth (see below); off on a Quest by default
# screen_effects = true  # occlusion, contact shadows and the screen's reflections in the eyes (see below)
```

`--xr` and `--no-xr` on the command line override `enabled` and `required`.
With each eye's picture a PC headset is given its depth, which makes the
picture steadier when the headset moves it to where the head has turned;
`--xr-depth=off` (or `depth = false`) runs without it, to compare. A
standalone headset is not given it unless the project says `depth = true`:
on a Quest 3 the depth cost more GPU time than it saved in frames.

**On a standalone headset each eye is drawn in one pass**, with two samples
a pixel to smooth its edges, straight into the image the headset shows: no
pass copies it and no anti-aliasing pass follows. A frame that needs the
picture or the depth partway through -- decals, particles, a surface shader
that reads the scene -- is drawn as on a PC, without the samples, and so is
a frame whose depth the headset or the room behind the picture reads.

`--xr-eyes-shot=<path>` writes both eyes' pictures as drawn, before the
headset's runtime has them, as `<name>-left.png` and `<name>-right.png`, at
the 600th frame the headset draws. The log says, once a session, where each
eye sits and how far it sees, and how much one eye is higher than the other
or turned from it: a pair of eyes that fuses has both near nought.
While a headset draws, the window shows what the left eye sees, cropped to
the window's shape: someone watching sees what the player sees. The OpenXR loader
ships beside the engine; nothing of it is loaded by a game that does not ask
for a headset.

**Shadows in a headset are the sun's shadow map**, one set for both eyes. It
is fitted from the head with a view as wide as the two eyes together, so
both eyes see the same shadow, texel for texel. Its first cascade ends two
meters out, where a player looks at what they hold and what is on the table
in front of them. The map is drawn once a frame, and the second eye reads
what the first drew. A standalone headset filters it with four reads, which
gives an edge two or three millimeters wide that stays attached where a cube
stands on a table. A flat game's sixteen-read filter, softened over five
centimeters, smeared it at half a meter and took most of the GPU's frame.
The shadow starts at the base of what casts it, under a millimeter away: an
eye's depth bias is two millimeters, where a flat game's is eight.
Occlusion, contact shadows and the screen's reflections and
bounced light are not drawn in the eyes. Each eye would mark them from its
own picture, so the two would disagree, which a pair of eyes sees as
shimmer. `screen_effects = true` draws them anyway. A game usually sets
`CastShadow = false` on what it draws for the local player's own head and
hands, whose shadows follow every movement.

## Where the player is

The headset's space is placed in the world by `Workspace.CurrentCamera`:
the camera's `CFrame` is where the play space's origin is -- on the floor
where the player stood when the session began -- and the head moves the
view from there. Move the player by moving the camera, or the character the
camera follows; never fight the headset for the head.

```luau
--!strict
local XRService = game:GetService("XRService")

if XRService.Enabled then
    local head = XRService:GetHeadCFrame()           -- in the play space, meters
    if XRService:IsHandTracked(Enum.XRHand.Right) then
        local hand = XRService:GetHandCFrame(Enum.XRHand.Right)
        local world = workspace.CurrentCamera.CFrame * hand
    end
end
```

With no headset, `Enabled` is false and every pose is the identity: a game
that plays both ways branches on `Enabled` once, where it places its camera.

A game played sitting down sets `XRService.Space = Enum.XRSpace.Seated` at
the start: poses are then measured from the head where the session began,
and the camera stands for the player's eyes instead of the floor.
`XRService:Recenter()` makes where the head is now the origin, facing where
it faces -- a "recentre" button in the game's menu.
`GetPlayAreaBounds()` is the width and depth of the room the player set up,
or nil. `UserCFrameChanged` fires when the head or a hand moves, and
`SessionState` says whether the player is in the game (`Focused`) or in the
headset's own menu (`Visible`): pause on anything but `Focused`.

```luau
--!strict
local XRService = game:GetService("XRService")

XRService.SessionStateChanged:Connect(function(state: EnumXRSessionState)
    local paused = state ~= Enum.XRSessionState.Focused
    workspace:SetAttribute("Paused", paused)
end)
```

## Interfaces

In a headset there is no screen to draw over, so each `ScreenGui` is shown
on a flat panel in front of the player instead -- 1.28 meters across at 1.5
meters by default -- and nothing about the interface has to change. Three
properties on the `ScreenGui` decide where:

- `XRDistance`: how far in front of the head, in meters.
- `XRPixelsPerMetre`: how many of its pixels make a meter. The panel is
  1280 by 720 pixels, or `ReferenceHeight` tall at 16:9.
- `XRFollow`: `Lazy` (the default) stays put until the head has turned 30
  degrees away from it; `Head` always follows; `Fixed` never does.

An interface somewhere precise -- a screen on a wall, a watch on a wrist --
is a `SurfaceGui` on a part, which already lives in the world.

The player points at interfaces with a hand -- the right one, or
`XRService.PointerHand` -- and its trigger, or a pinch, is a click: on the
panels, and on every `SurfaceGui` and `BillboardGui`, as a mouse's click is.
A beam is drawn from the hand to where it points (`PointerVisible`,
`PointerLength`). With hand tracking, a button in reach is also pressed by
pushing a fingertip into it, and released by drawing the finger back. A game that points at things of its own -- a gun, a
grabbed object -- casts the same ray:

```luau
--!strict
local XRService = game:GetService("XRService")

local from, along = XRService:GetPointerRay(Enum.XRHand.Right)
if from and along then
    local hit = workspace:Raycast(from, along * 20)
end
```

## Moving without being sick

A player who walks with a stick while standing still is the main cause of
discomfort in VR. `@engine/xrcomfort` gives a game the usual answers in one
call:

```luau
--!strict
local xrcomfort = require("@engine/xrcomfort")

local session = xrcomfort.start({
    Move = "teleport", -- or "smooth"
    Turn = "snap", -- or "smooth"
    SnapDegrees = 30,
    Vignette = 0.6, -- how much of the view closes while moving smoothly
})
```

- **Teleport**: the left stick pushed forward draws an arc from the left
  hand; released, the view fades for a tenth of a second and the player is
  where the arc landed -- only on something flat enough to stand on.
- **Snap turn**: the right stick pushed to a side turns the player by
  `SnapDegrees`, about their head, at once.
- **Smooth move and turn**, with the edges of the view darkened while
  moving.
- **Seated** raises a seated player to a standing player's eyes, and
  `session:CalibrateHeight()` makes where the head is now that height.

The session moves the camera from then on: a game moves its player by
setting `session.Origin`.

## Hands

On a headset that tracks hands -- the Quest 3 does -- a player who puts the
controllers down plays with their hands. `GetHandCFrame` then follows the
palm, and a pinch of the thumb and the index finger is the `XRPinchLeft` or
`XRPinchRight` key, so a "select" action bound to the trigger can bind the
pinch beside it. `GetHandJoints` returns the hand's 26 joints for a game
that draws or reads the fingers. A game played only with controllers says
`hands = false` under `[xr]`.

## The controllers are input

Each controller's buttons, triggers, grips and sticks are `Enum.KeyCode`
values whose names begin with `XR`, bound to `InputAction`s like any key --
so a game's actions take them beside a gamepad's:

```luau
--!strict
local jump = Instance.new("InputAction")
for _, key in { Enum.KeyCode.ButtonSouth, Enum.KeyCode.XRButtonA } do
    local binding = Instance.new("InputBinding")
    binding.KeyCode = key
    binding.Parent = jump
end
```

| Key code | Is |
|---|---|
| `XRButtonA`, `XRButtonB` | The right controller's face buttons. |
| `XRButtonX`, `XRButtonY`, `XRMenu` | The left controller's. |
| `XRTriggerLeft`, `XRTriggerRight`, `XRGripLeft`, `XRGripRight` | From 0 to 1; a `Bool` action counts them pressed past half. |
| `XRThumbstickLeft`, `XRThumbstickRight` | Both axes, for a `Direction2D` action; `...X` and `...Y` alone. |
| `XRThumbstickLeftClick`, `XRThumbstickRightClick` | A stick pressed in. |

The engine suggests where each is on Meta Touch, Valve Index, HTC Vive and
the simple controller; a runtime that lets the player rebind them is obeyed.
`InputService.LastInputDeviceType` is `Enum.InputDeviceType.XR` after a
controller was used.

**A controller vibrates** through `HapticService`, as a gamepad does:

```luau
HapticService:SetMotor(Enum.UserInputType.XRRight, Enum.VibrationMotor.Large, 0.6)
```

## The room around the player

On a headset with cameras -- the Quest 3 sees in color -- a game can show
the player's real room behind what it draws:

```luau
--!strict
local XRService = game:GetService("XRService")

if XRService:SupportsPassthrough() then
    XRService.Passthrough = true
end
```

The room shows wherever the game draws nothing; the sky is not drawn. A game
for augmented reality builds only what stands in the room -- a creature on
the floor, a board on the table. `Lighting.XRPassthroughOpacity`, from 0 to
1, fades the game's world over the room, for a portal or a transition.

Where the headset knows the room -- the Quest's scene, which the player
captures once in its settings -- a game can ask for it, and make it solid:

```luau
--!strict
local XRService = game:GetService("XRService")

local room = XRService:GetRoomAsync()
if room then
    for _, plane in room.Planes do
        print(plane.Kind, plane.CFrame.Position, plane.Size)
    end
    XRService:CreateRoomParts()
end
```

Each plane is a rectangle in the play space, facing into the room. The
first time a game asks, the Quest asks the player whether it may see the
room; the answer is nil when they say no. `CreateRoomParts` puts an
invisible, anchored part behind each plane in `Workspace.XRRoom`, so a ball
bounces on the real table and a raycast finds the real wall. The simulated
headset has a room of its own for a test: a floor, a ceiling, four walls and
a table.

An anchor pins a place in the room, and the headset keeps it there as it
learns the room better. Saved, it is found again in the next session, in the
same room -- a board left on the real wall is on it tomorrow:

```luau
--!strict
local XRService = game:GetService("XRService")

local saved = XRService:LoadAnchorsAsync()
if #saved == 0 then
    local anchor = XRService:CreateAnchorAsync(CFrame.new(0, 1.5, -1))
    if anchor then
        anchor:SaveAsync()
    end
end
```

`anchor.CFrame` is where it is now, in the play space, and `anchor.Id` tells
saved anchors apart. Destroying an `XRAnchor` lets the headset's anchor go;
what was saved stays saved.

## On the Quest, standalone

The Quest runs the game on its own, from an APK:

```
ludwerk build --target=quest
```

It is the Android build -- the same `[export.android]` settings and key --
with the OpenXR loader beside the game and a manifest that starts it in the
headset rather than in a flat window. Install it with `adb install -r` with
the Quest connected and in developer mode; it appears among the headset's
apps. A game needs `[xr] enabled = true` to be built for the Quest.

## Testing without a headset

`--xr=simulated` runs a game with a headset that is the engine's own: two
eyes 63 mm apart standing at 1.6 m, in a room three meters by three,
headless too. With `--screenshot`, each eye's picture is written beside the
window's (`-left`, `-right`). A test moves its head and hands from a script,
in the play space, from the next frame:

```luau
--!strict
local XRService = game:GetService("XRService")

XRService:SimulatePose(Enum.XRPose.Head, CFrame.new(0, 1.7, 0.5))
XRService:SimulatePose(Enum.XRPose.RightHand, CFrame.new(0.25, 1.2, -0.3))
```

`SimulatePose` raises an error anywhere but under the simulated headset.
`--xr-simulated-store=<file>` keeps the anchors a test saves in a file, for
a second run to load.
