# VR

A game drawn in a headset, through OpenXR: on a PC with a headset its
runtime drives (SteamVR, a Quest through Link), each eye drawn at the
headset's rate. Without a headset the same game is drawn on the screen.

## Turning it on

In `project.toml`:

```toml
[xr]
enabled = true     # try a headset at start; the screen when there is none
# required = true  # a game played only in a headset: without one it says so and does not start
```

`--xr` and `--no-xr` on the command line override both. The OpenXR loader
ships beside the engine; nothing of it is loaded by a game that does not ask
for a headset.

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
