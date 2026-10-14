# Clicks, and the prompts and drags a game writes

A button to press wants a raycast from the pointer, a distance check, a hover
state, touch handling and, in a match, a message to the server that the server
has to check. A `ClickDetector` does all of it for you.

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

## A prompt, or a drag, is your game's

A "hold E to open" prompt and a drawer pulled by the pointer are design, and
the engine leaves them to the game -- each is a few lines, and they are yours
to shape. `examples/30-interactions` writes both:

- **A prompt**: on the authority, each tick, a player within reach of the door
  whose `Use` action has been held long enough opens it; on the client, a
  `BillboardGui` over the door says what holding E will do while the local
  character is in reach.
- **A drag**: a `ClickDetector` click takes hold and the next lets go; while
  holding, the client sends the pointer's ray (`Camera:ViewportPointToRay`) on
  a `RemoteEvent`, and the authority works out where the drawer or the lever
  goes -- along its runners, about its hinge -- within its limits.

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
- A click is resolved from the camera and the viewport as the tick begins, so a
  replay reproduces it -- but the replay harness does not record the pointer
  yet.

`examples/30-interactions` is a room with a button, a door, a drawer and a
lever: the button the engine's, the rest the game's own.
