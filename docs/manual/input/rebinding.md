# Rebinding and prompts

Rebinding works because of how the system is shaped rather than because of a
feature bolted onto it: an `InputBinding` is an instance, and changing what an
action responds to is assigning a property.

```luau
--!strict
local binding = jump:GetPreferredBinding(Enum.InputDeviceType.KeyboardMouse)
if binding ~= nil then
    binding.KeyCode = Enum.KeyCode.J
end
```

That is the whole mechanism. No registration, no re-binding call, no cache to
invalidate — the next tick resolves against the new key.

## Adding and removing a binding

An action can have any number of bindings, and they are ordinary children:

```luau
--!strict
local extra = Instance.new("InputBinding")
extra.KeyCode = Enum.KeyCode.Up
extra.DeviceType = Enum.InputDeviceType.KeyboardMouse
extra.Parent = jump
```

Destroy one to remove it. An action with no bindings resolves to its type's
zero — false, or a zero vector — rather than erroring.

## Finding the binding to show

`InputAction.GetPreferredBinding` takes an optional
`Enum.InputDeviceType` and returns the binding a prompt should show. With no
argument it answers for the device the player is currently using.

```luau
--!strict
local InputService = game:GetService("InputService")

local function promptFor(action: InputAction): string
    local binding = action:GetPreferredBinding(InputService.LastInputDeviceType)
    if binding == nil then
        return ""
    end
    if binding.DisplayName ~= "" then
        return binding.DisplayName
    end
    return binding.KeyCode.Name
end
```

`InputService.LastInputDeviceType` is what a game watches to decide whether to
draw a key or a gamepad glyph, and `InputService.InputDeviceChanged` fires when
it changes — which is the moment to redraw every prompt on screen.

## The two properties a prompt reads

`InputBinding.DisplayName` is a human-facing name for the binding, for the cases
where the key code's own name is wrong or unhelpful. Empty means the engine has
nothing better to offer and the prompt should fall back to the key's own name.

`InputBinding.Image` is a glyph, as an `asset://` URI. Empty means none.

**The engine never acts on either, and never will.** That is what makes them
different from a property that is stored and not yet implemented: this is data a
*game* reads back to draw its own prompt.

There is no prompt widget here, and there is not meant to be one. A rebinding
screen is a game's screen — it knows its own layout, its own fonts and its own
idea of what a key looks like.

## Devices

`InputBinding.DeviceType` says which family a binding belongs to, which is what
lets `GetPreferredBinding` answer sensibly for a player holding a gamepad.

`Enum.InputDeviceType` covers keyboard-and-mouse, gamepad and touch. Touch has
no hardware behind it in this release: what produces it is
`InputService.SetVirtualState`, which is how an on-screen control drives an
action — see [Buttons and interaction](manual:ui/interaction).

## Saving a scheme

There is no built-in serialization for a control scheme, and it does not need
one: a binding's state is a handful of enum values and a scale.

```luau
--!strict
local function schemeOf(action: InputAction): { { Key: string, Scale: number } }
    local out = {}
    for _, child in action:GetChildren() do
        if child:IsA("InputBinding") then
            local binding = child :: InputBinding
            table.insert(out, { Key = binding.KeyCode.Name, Scale = binding.Scale })
        end
    end
    return out
end
```

Where that gets written is your backend — see
[Talking to a backend](manual:guides/backend).

## Where to look next

- [Actions, bindings and contexts](manual:input/actions)
- [`InputBinding`](api:InputBinding) · [`InputService`](api:InputService)
## Controller family and prompt selection

`InputService.PreferredInput` is an alias of `LastInputDeviceType`; both use
`Enum.InputDeviceType`. `InputDeviceChanged` is their existing category signal.
`PreferredGamepadType` and `PreferredGamepadId` describe the last used controller,
independently of the operating system. Connecting a controller does not select it.

```luau
local input = game:GetService("InputService")
if input.PreferredInput == Enum.InputDeviceType.Gamepad then
    if input.PreferredGamepadType == Enum.GamepadType.PlayStation then
        -- Select the game's PlayStation prompt artwork for the current binding.
    end
end
input.PreferredGamepadTypeChanged:Connect(function(family)
    -- Refresh prompts when the physical controller family changes.
end)
```

Families are Xbox, PlayStation, Nintendo, Generic and Unknown. Provide a neutral
fallback for Generic and Unknown. Family is a prompt hint, not a button layout:
resolve the current binding before selecting artwork, and allow remapped layouts.
No glyph assets or `GetInputIcon` method are provided by this extension.

`PreferredGamepadIdChanged` also covers switching between controllers of the
same family. `GamepadConnected` and `GamepadDisconnected` carry `(id, family)`.
Zero ID means no selected controller. Keyboard, mouse or touch use changes the
category without discarding controller metadata; unplugging the selected pad
clears its ID and family. Drift below the preference threshold does not steal
the category from keyboard/mouse. Signals use the normal deferred event system.
