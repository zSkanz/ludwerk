# Clicks and proximity prompts

A button to press, a door to open, a crate to pick up: three things nearly
every game needs, and each one wants a raycast from the pointer, a distance
check, a hover state, touch handling and, in a match, a message to the server
that the server has to check. Two instances do all of it for you.

## `ClickDetector`

Put a `ClickDetector` in a part, or in a model, and the part can be clicked:

```luau
--!strict
local button = workspace:WaitForChild("Button")
local detector = Instance.new("ClickDetector")
detector.MaxActivationDistance = 8
detector.Parent = button

detector.MouseClick:Connect(function(player: Player)
    print(`{player.Name} pressed the button`)
end)
```

- Once a tick the engine casts the pointer -- the mouse, or a tap -- into the
  world. The nearest part hit is the one pointed at; a detector in it, or in a
  model around it, is the one that answers. A part with `CanQuery` off is
  passed through, as a raycast passes through it.
- `MaxActivationDistance` is measured from the player's character, or from the
  camera when there is none.
- `MouseClick` and `RightMouseClick` fire for the two buttons, and a tap is a
  `MouseClick`. `MouseHoverEnter` and `MouseHoverLeave` follow the mouse, on
  that machine only.

## `ProximityPrompt`

Put a `ProximityPrompt` in a part, an attachment or a model, and a prompt
appears when a player comes near:

```luau
--!strict
local door = workspace:WaitForChild("Door")
local prompt = Instance.new("ProximityPrompt")
prompt.ActionText = "Open"
prompt.ObjectText = "Door"
prompt.HoldDuration = 0.5
prompt.Parent = door

prompt.Triggered:Connect(function(player: Player)
    print(`{player.Name} opened the door`)
end)
```

- It shows within `MaxActivationDistance` of the character, and only in sight
  of it unless `RequiresLineOfSight` is off.
- It is triggered by `KeyboardKeyCode` (E), `GamepadKeyCode` (X on the
  standard layout), or a tap on the prompt. With a `HoldDuration`, the key is
  held that long -- `PromptButtonHoldBegan`, a ring filling, then `Triggered`
  -- and letting go fires `TriggerEnded`.
- **Several in reach**: with `Exclusivity = OnePerButton` only the nearest on
  each key shows, with `OneGlobally` only the nearest of those, and
  `AlwaysShow` always shows. `ProximityPromptService.MaxPromptsVisible` caps
  them all, and `ProximityPromptService.Enabled = false` hides every one -- a
  cutscene, a menu.
- **The look is the engine's**: a dark box with the key and the two texts.
  `Style = Custom` draws nothing, and `PromptShown(inputType)` and
  `PromptHidden` are yours to draw your own on.
- **A game's own keys come first.** The prompt's keys go through an input
  context the engine owns, below every context a game makes: a context of yours
  that sinks E takes E away from the prompts.

`ProximityPromptService.PromptTriggered(prompt, player)` fires for any of them.

## In a match

Each machine resolves its own pointer and keys, and fires the signal at once,
so a client's scripts react without waiting. It also tells the server, which
checks that the player's character could have reached the thing -- the
detector's reach from where the server has that character -- and fires the
signal there with that player. **The player is the connection's**, never
anything the client says, so a forged click from across the map does nothing.
Hovering is never sent.

## Limits

- `CursorIcon` is stored and not drawn yet: the pointer keeps its look.
- The prompt's `UIOffset` is not sent to a client; set it on the machine that
  draws the prompt.
- A click is resolved from the camera and the viewport as the tick begins, so a
  replay reproduces it -- but the replay harness does not record the pointer
  yet.

`examples/30-interactions` is a room with a button and a door.
