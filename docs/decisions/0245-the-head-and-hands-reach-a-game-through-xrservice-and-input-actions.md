# 0245 — The head and hands reach a game through `XRService` and its input actions

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0244 (OpenXR), the input actions and contexts
  (`InputAction`, `InputContext`, `InputBinding`), `HapticService`

## The question

A game in a headset needs where the head and the two hands are, what the
controllers' buttons and sticks do, haptics, and -- on a Quest 3 -- the hands
themselves, joint by joint, when no controller is held. It should get them
the way it already gets input, so that the same game plays flat with a
gamepad and in a headset with controllers, and a script that never mentions
XR still works in one.

## Decision

### 1. `XRService`: where the player is

```luau
--!strict
local XRService = game:GetService("XRService")

if XRService.Enabled then                          -- a headset is running this game
    local head = XRService:GetHeadCFrame()          -- in the play space, metres
    local left = XRService:GetHandCFrame(Enum.XRHand.Left)
    local joints = XRService:GetHandJoints(Enum.XRHand.Right) -- 26 CFrames, or nil without hand tracking
    XRService:Recenter()
    XRService.UserCFrameChanged:Connect(function(which: Enum.XRPose, cframe: CFrame) end)
end
```

Poses are in the **play space**, whose origin is on the floor where the player
stood when the session began (`Enum.XRSpace.Standing`, OpenXR's STAGE) or at
the head's height for a seated game (`Seated`, LOCAL); `XRService.Space` chooses
it, and `GetPlayAreaBounds()` returns the floor's guardian rectangle where
the runtime gives one. **The play space is placed in the world by
`Workspace.CurrentCamera`**: the camera's `CFrame` is where the play space's
origin is, and the head moves the view from there. A game moves its player by
moving the camera (or the character the camera follows) and never fights the
headset for the head.

The poses a script reads are the ones of the tick it runs in; the picture
uses later ones (ADR 0244 §3), so a hand drawn in the player's view is where
the hand is, and a hand a script raycasts from is where it was a few
milliseconds earlier -- the same split the engine already makes between the
simulation and what is drawn.

### 2. Controllers are input actions

Each controller's buttons, triggers, grips, sticks and touch sensors are
`Enum.KeyCode` values (`XRButtonA`, `XRTriggerLeft`, `XRThumbstickRight`, ...),
bound to `InputAction`s like any key -- so a game's existing actions bind them
beside the gamepad's:

```luau
local jump = Instance.new("InputAction")
local binding = Instance.new("InputBinding")
binding.KeyCode = Enum.KeyCode.ButtonA   -- the gamepad's
binding.Parent = jump
local inXr = binding:Clone()
inXr.KeyCode = Enum.KeyCode.XRButtonA    -- the right controller's A
inXr.Parent = jump
```

Underneath, the engine makes one OpenXR action set with every such input for
the profiles it knows (Meta Touch Plus and Touch Pro, Valve Index, HTC Vive,
Khronos simple controller), suggests the bindings to the runtime, and reads
the actions each frame; a runtime that rebinds them for the player is obeyed.
A pose is not an input action: it is `XRService`'s.

### 3. Haptics through `HapticService`

`HapticService:SetMotor(Enum.UserInputType.XRLeft, Enum.VibrationMotor.Large,
strength)` vibrates a controller as it does a gamepad. OpenXR's haptic output
takes a duration and an amplitude, and the engine renews it while a motor is
set.

### 4. Hand tracking

With `XR_EXT_hand_tracking` (the Quest 3 has it), `GetHandJoints` returns the
26 joints of OpenXR's hand, and a pinch of thumb and index finger is reported
as `XRPinchLeft` and `XRPinchRight` key codes, so a game's "select" action
works with a hand as with a trigger. A game that only wants controllers says
`[xr] hands = false`.

### 5. Flat, the same game

With no headset, `XRService.Enabled` is false, every pose is the identity,
`GetHandJoints` returns nil, and nothing else changes. A game written for both
branches on `Enabled` once, where it places its camera.

## Proof

- The simulated headset (ADR 0248) moves the head and hands along a recorded
  path: `GetHeadCFrame` and `GetHandCFrame` follow it within a millimetre and
  a tenth of a degree, in both spaces.
- Its trigger, bound to an action beside the gamepad's `ButtonR2`, fires the
  action; the gamepad still does.
- A recorded hand pinches: `XRPinchRight` fires, and 26 joints are returned.
- On the Quest 3 through Link (the maintainer's): the example's hands and grab.
- A game with no `[xr]`: `XRService.Enabled` false and nothing loaded.

## Consequences

- New API: `XRService` (`Enabled`, `Space`, `GetHeadCFrame`,
  `GetHandCFrame`, `GetHandJoints`, `GetPlayAreaBounds`, `Recenter`,
  `UserCFrameChanged`, `SessionState`, `SessionStateChanged`), `Enum.XRHand`,
  `Enum.XRPose`, `Enum.XRSpace`, `Enum.XRSessionState`; `KeyCode` values for
  controllers and pinches; `UserInputType.XRLeft` and `XRRight`.
- Poses are presentation for the world's hash: they enter the simulation only
  through what a script does with them, which is replicated as any change is.

## Not decided here

UI and pointing (ADR 0246), passthrough (ADR 0247), sending the head and hands
to other players (ADR 0248), eye tracking (no runtime the plan names offers it
to a game without a special agreement), and full-body tracking.

## As built, first step (2026-10-18)

- `XRService.Enabled`, `GetHeadCFrame`, `GetHandCFrame` and `IsHandTracked`
  (where the hand holds its controller), from what the host read of the
  headset at the start of the frame, for the ticks after it. Poses are the
  engine's state as presentation, never hashed.
- The controllers are one OpenXR action set -- grip and aim poses, trigger,
  squeeze, thumbstick and its click, the two face buttons, the menu and the
  haptic output -- with bindings suggested for Meta Touch, Valve Index, HTC
  Vive and the simple controller, and they reach the input system as the
  nineteen `XR` key codes (152 to 170), so an `InputBinding` takes one as it
  takes a key. `Enum.InputDeviceType.XR`; `Enum.UserInputType.XRLeft` and
  `XRRight`, which `HapticService:SetMotor` vibrates, renewed each frame.
- **Not yet**: `Space` and `Recenter`, `GetPlayAreaBounds`, the events,
  `SessionState`, hand tracking and the pinches (their key codes are
  reserved), and `XRService:SimulatePose` -- the simulated headset is driven
  from C++ in tests until then.

## As built, second step (2026-10-18)

- `XRService.Space` (`Enum.XRSpace`): both of OpenXR's spaces are made at
  the start -- the stage, where the runtime has a floor, and local -- and the
  poses and the eyes are located in the one chosen, from the next frame.
  Standing without a stage is local.
- `Recenter` is the engine's, not the runtime's: where the head is at the
  next frame -- across the floor, and its turn about the vertical -- becomes
  the origin of what scripts read and of the picture (the camera, then the
  recentre, then the head). A recentre belongs to its space, and changing
  `Space` drops it.
- `GetPlayAreaBounds` from `xrGetReferenceSpaceBoundsRect` on the stage;
  `SessionState` (`Enum.XRSessionState`) from the session's state, with
  `SessionStateChanged`; `UserCFrameChanged` once a frame for each pose that
  moved.
- `SimulatePose` (`Enum.XRPose`) puts the simulated headset's head or a hand
  in the play space the script reads; the simulated headset is seated 1.6 m
  under its standing origin and has a room of 3 by 3 metres.
- **Not yet**: hand tracking and the pinches.
