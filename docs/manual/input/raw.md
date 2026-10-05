# Raw input, and when to use it

`InputService` carries a raw event surface beside the Input Action System. It is
the direct, familiar option, and it is the right one for a prototype, a debug
key, or anything a player will never remap.

**It is not the one a shipped game should be built on.** An action is what can be
rebound, prompted and recorded; a key code is not.

## Polling

```luau
--!strict
local InputService = game:GetService("InputService")

if InputService:IsKeyDown(Enum.KeyCode.LeftShift) then
    -- sprinting
end
```

`InputService.IsKeyDown` is whether that key, button or trigger is held as of the
current tick. An analogue source counts as down past half deflection — the same
rule a boolean action applies.

**It reads the device snapshot and ignores what the UI consumed**, which is the
opposite of what the events below do. A poll asks what the hardware is doing; an
event asks what happened to the game. If you want the UI-aware answer, you want
an `InputAction`.

## The events

```luau
--!strict
InputService.InputBegan:Connect(function(input: InputObject, uiConsumed: boolean)
    if uiConsumed then
        return
    end
    if input.KeyCode == Enum.KeyCode.F1 then
        toggleDebugPanel()
    end
end)
```

| Event | Fires on |
|---|---|
| `InputService.InputBegan` | A key, button or touch going down. |
| `InputService.InputChanged` | Movement: pointer, wheel, an analogue axis. |
| `InputService.InputEnded` | The same input going up. |

Each carries an `InputObject` and a `uiConsumed` boolean.

**These events come from the Input Action System's own dispatch, not from the
operating system.** Same source, same tick, after the UI has taken what it took
— and in a replay they come from the recorded stream, so a recorded run
reproduces every input a game reads.

They fire on the **simulation** clock, so a handler that writes to the world
replays exactly. Camera look at render rate is an `InputContext` with
`Rate = Enum.InputRate.Render`, not a raw handler.

`uiConsumed` on `InputEnded` carries what it carried when the input began, so a
press that started on a button is still marked consumed when it is released off
one.

## InputObject

A read-only snapshot of one input on one tick. A handler that stashes one is
holding a fact rather than a handle.

| Property | Means |
|---|---|
| `InputObject.KeyCode` | Which key, button or axis. |
| `InputObject.UserInputType` | What kind of input it was. |
| `InputObject.Position` | The pointer in window pixels, origin top-left, with `z` carrying accumulated wheel. For a gamepad axis, the deflection instead. |
| `InputObject.Delta` | The change since the last event. |
| `InputObject.TouchId` | For a touch, which finger -- the same number from its `InputBegan` to its `InputEnded`. Zero otherwise. |

`Position` is a three-wide vector rather than a `Vector2` because that is the
engine's native primitive — no userdata and no allocation, which matters on a
value produced several times a tick.

## Touch

On a touchscreen every finger is an input of its own: `InputBegan` when it
lands, `InputChanged` as it moves, `InputEnded` when it lifts, all with
`Enum.UserInputType.Touch`, `Position` in window pixels and a `TouchId` that
says which finger. A tap shorter than a frame still begins and ends, one tick
apart.

**On-screen controls are the game's**: it draws the buttons, follows each
finger by its `TouchId`, and drives its actions through
`InputService:SetVirtualState` -- so a binding to `Enum.KeyCode.VirtualStick1`
or `Virtual3` moves and jumps exactly as the keyboard does, and the controller
never learns which it is being played with:

```luau
--!strict
local fingers: { [number]: string } = {}

InputService.InputBegan:Connect(function(input: InputObject)
    if input.UserInputType == Enum.UserInputType.Touch then
        fingers[input.TouchId] = if input.Position.x < 400 then "left" else "jump"
        InputService:SetVirtualState(Enum.KeyCode.Virtual3, if fingers[input.TouchId] == "jump" then 1 else 0)
    end
end)
```

`examples/20-platformer` is this at full size: three buttons that appear the
first time a finger touches the screen, two thumbs at once, and a thumb that
slides from left to right turning around.

There are sixteen virtual keys, `Virtual1` to `Virtual16`. The first four are
also the halves of `VirtualStick1` and `VirtualStick2`; the rest are for the
buttons a HUD grows.

**A finger that lands on the interface says so.** A tap presses the
interface's buttons as a click does, and that finger's `InputBegan`, every
`InputChanged` and its `InputEnded` arrive with the second argument true --
the same `uiConsumed` a click on a button carries. A game that also aims by
tapping the world checks it:

```luau
--!strict
InputService.InputBegan:Connect(function(input: InputObject, uiConsumed: boolean)
    if uiConsumed or input.UserInputType ~= Enum.UserInputType.Touch then
        return -- a button was under the finger
    end
    aimAt(input.Position)
end)
```

## The back button of a phone

**On Android, the back button and the back gesture are `Enum.KeyCode.Escape`**
(ADR 0170): pressed and released like the key, heard by `InputBegan`, by
`IsKeyDown` and by any `InputAction` bound to Escape. The system does not
close the game for it. A game that closes a menu or opens its pause on Escape
does the same on a phone with nothing changed.

```luau
--!strict
local InputService = game:GetService("InputService")

InputService.InputBegan:Connect(function(input: InputObject)
    if input.KeyCode ~= Enum.KeyCode.Escape then
        return
    end
    if sheetIsOpen() then
        closeSheet()
    elseif atTheFirstScreen() then
        game:Shutdown() -- what back means where there is nothing to close
    else
        openPause()
    end
end)
```

**Leaving is yours to do**: at the first screen, where back has nothing to
close, a phone's player expects to leave, and `game:Shutdown()` is how. The
engine does not guess which screen that is.

## Gestures

A swipe, a tap, a press held, two fingers closing, a drag: the engine
recognises them, from a finger and from the mouse with its left button down
alike, so a game written on a desk is the game on a phone.

| Signal | Says |
|---|---|
| `InputService.TouchSwiped(direction, start, fingers)` | a finger travelled `SwipeThreshold` along one axis |
| `InputService.TouchTapped(position)` | down and up again where it landed, within a third of a second |
| `InputService.TouchLongPressed(position)` | held where it landed for half a second |
| `InputService.TouchPinched(scale, centre)` | two fingers apart or together: the distance over what it was when the second landed |
| `InputService.TouchPanned(delta, fingers)` | what is down moved, this tick, in window pixels |

**A swipe is also a key.** `Enum.KeyCode.SwipeUp`, `SwipeDown`, `SwipeLeft` and
`SwipeRight` bind to an `InputAction` like any other, as a press that lasts one
tick -- so "move left" is the `A` key and a swipe left in the same action, and
the code that moves the piece never asks which:

```luau
--!strict
for _, code in { Enum.KeyCode.A, Enum.KeyCode.Left, Enum.KeyCode.SwipeLeft } do
    local binding = Instance.new("InputBinding")
    binding.KeyCode = code
    binding.Parent = moveLeft
end
moveLeft.Pressed:Connect(slideLeft)
```

Three rules:

- **It is said while the finger is still down**, as soon as it has gone far
  enough -- not when it lifts.
- **One long drag one way is one swipe.** A change of direction is the next,
  measured from where the finger then is.
- **A press the interface took starts none** (`UIObject.Active`).

`InputService.SwipeThreshold` is the length, in **millimetres** of screen -- 6
by default. A length under a thumb is the same on a phone and on a monitor; a
number of pixels would be a nudge on one and a reach across on the other.

## What the machine has

`InputService.LastInputDeviceType` says what was used last. Before anything has
been used -- a script's first line, where a game decides which HUD to build --
ask what is there:

| Property | True when |
|---|---|
| `InputService.TouchAvailable` | the machine has a touchscreen |
| `InputService.KeyboardAvailable` | a keyboard is attached |
| `InputService.GamepadAvailable` | at least one gamepad is connected |

`RunService.Platform` is the operating system (`Enum.Platform`), for the few
things that differ by system. What a game shows should follow what the machine
has, not what it is called: a tablet with a keyboard and a laptop with a
touchscreen are both real.

**A gamepad that was plugged in before the game started is connected a frame
after it starts**, not at the first line of a script: the engine looks for
gamepads once its first frame is on the screen, so that a window is never
later for it. `GamepadAvailable` read at a script's first line is false, and
its changed signal says when that is no longer so -- connect to it rather
than read it once.

**A touch's `Position` is in window pixels**, and a `ScreenGui` with
`ScreenInsets` on starts at the safe area. Something a game places at a finger
inside such a screen is off by the inset: subtract the `AbsolutePosition` of a
full-size frame in it, or give that screen `ScreenInsets = false`.

## The pointer

```luau
--!strict
local where = InputService:GetPointerPosition()

InputService.PointerLocked = true    -- captured for mouse-look
InputService.PointerVisible = false
```

`InputService.PointerLocked` captures the pointer for relative motion, which is
what a first-person camera wants. `InputService.PointerVisible` shows or hides
the cursor. They are separate because hiding a cursor and capturing it are
different decisions.

## The clipboard

```luau
copyButton.Activated:Connect(function()
    InputService:SetClipboard(roomCode)
end)
```

`InputService:SetClipboard(text)` puts text on the player's clipboard: a
room's code, a seed, a link. Call it from a client script, where the player
is; on a server there is no clipboard and nothing happens. It takes up to
64 KiB.

There is no way to read the clipboard. What a player copied elsewhere is
theirs, and a `TextInput` already pastes where they choose to.

## Window focus

```luau
InputService.WindowFocusChanged:Connect(function(focused: boolean)
    if not focused then
        pauseGame()
    end
end)
```

Losing focus also clears held state, so a key held when the player alt-tabbed
does not stay held forever.

## When to reach for this

- A debug key that will never ship.
- A prototype before the control scheme exists.
- Something genuinely device-specific — reading raw wheel deltas, say.

And when not to: anything a player might want to rebind, anything a gamepad
should also do, anything a prompt should be able to describe, and anything that
must survive being replayed.

## Where to look next

- [Actions, bindings and contexts](manual:input/actions) — the model to prefer
- [`InputService`](api:InputService) · [`InputObject`](api:InputObject)
