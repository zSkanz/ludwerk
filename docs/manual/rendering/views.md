# Views: cameras on screens

A security monitor, a rear-view mirror, a scope, a minimap, a television in a
bar: each is the world drawn from another camera and shown on something. A
`CameraTexture` is that: a camera drawing into a texture with a name, and
anything that takes a texture can show the name.

## A camera on a screen

```luau
--!strict
local lobby = Instance.new("Camera")
lobby.CFrame = CFrame.lookAt(vector.create(0, 5, 10), vector.create(0, 1, 0))
lobby.Parent = workspace

local feed = Instance.new("CameraTexture")
feed.Camera = lobby
feed.ViewName = "lobby"                 -- draws into view://lobby
feed.Resolution = Vector2.new(320, 180)
feed.Parent = lobby

-- Anything that takes a texture shows it: a picture on a wall...
local screen = Instance.new("ImageLabel")
screen.Size = UDim2.fromScale(1, 1)
screen.Image = "view://lobby"
screen.Parent = monitorSurfaceGui
```

**`view://<name>`** is accepted wherever a texture is: an `ImageLabel`'s or an
`ImageButton`'s `Image` (in a `ScreenGui`, a `SurfaceGui` or a
`BillboardGui`), a `Decal`'s `Texture`, and a material's maps. Until the first
picture, the texture is black -- a screen that is off.

**A picture shows truest on an `ImageLabel`.** The texture holds the finished
picture, colours already encoded for a screen; a `Decal` or a material reads
its map as a colour to be lit, and the feed comes out lighter than it was
drawn. For a monitor, put the picture on a `SurfaceGui`.

| Property | What it does |
|---|---|
| `Camera` | The camera it draws from. Nil draws nothing and keeps the last picture. |
| `ViewName` | The name after `view://`. Two with one name: the first made draws, and the second says so in the log. |
| `Resolution` | The texture's size in pixels, capped at `[render] max_view_resolution` on its longer side, the shape kept. |
| `Enabled` | Off keeps the last picture and costs nothing. |
| `UpdateInterval` | Draw on one frame in this many. |
| `Quality` | `Simple` (the default) draws without shadows or the look's effects; `Full` draws as the main view does. |

A feed is drawn **before** the main view, so a monitor in the world shows this
frame's picture. A camera that sees a surface showing its own texture sees that
surface without it -- there is no recursion to worry about, and a mirror in a
mirror is for later. Each camera keeps **its own exposure**:
a camera in a dark room opens up, the way a real one does, whatever the main
view is looking at.

## Instances in the UI: `ViewportFrame`

An item turning in an inventory slot, a character preview, a 3D icon: a
`ViewportFrame` is a UI element that draws the parts and models inside it.

```luau
--!strict
local slot = Instance.new("ViewportFrame")
slot.Size = UDim2.fromOffset(96, 96)
slot.BackgroundTransparency = 1         -- only the item shows
slot.Parent = inventory

local keycard = Instance.new("Part")
keycard.Size = vector.create(1.6, 1, 0.08)
keycard:SetMaterialParameter("Color", Color3.fromRGB(230, 70, 60))
keycard.Parent = slot

RunService.Heartbeat:Connect(function()
    keycard.CFrame = CFrame.fromEuler(0.35, os.clock(), 0)
end)
```

- **What is inside is its world and nobody else's**: drawn in the frame and
  nowhere else, and not simulated -- no gravity, no collisions. A script
  moves it by setting `CFrame`.
- **A clip plays in it**: an `AnimationPlayer` under a skinned mesh inside a
  frame animates it, as it would in the world -- a hero standing in its idle
  in a selection screen. A small frame's mesh is posed less often than a
  large one's, as a far figure is.
- **Framed for you**: with no `CurrentCamera`, it looks at everything inside
  from the front and a little above. Put a `Camera` inside it and set
  `CurrentCamera` to choose the angle yourself.
- **Its own simple light**: `Ambient`, and one light of `LightColor` travelling
  along `LightDirection`. No sky and no shadows; with
  `BackgroundTransparency = 1`, only what is inside shows, over whatever is
  behind the frame.
- **Drawn only when something changes**: an item moved, a clip posed it, its
  camera, its light, its size. Forty still items are forty pictures once; one
  turning, or one breathing, is one picture a frame.

## Mirrors and portals

```luau
--!strict
local views = require("@engine/views")

views.mirror(workspace.Bathroom.Mirror)                     -- a mirror on its front face
views.portal(workspace.BlueDoor, workspace.OrangeDoor)      -- in the blue, out of the orange
```

- **`views.mirror(part)`** puts the picture on the part's front face and,
  every tick, moves a camera to where `workspace.CurrentCamera` would be
  reflected through the glass. What stands in front of the glass is in it,
  turned the way a mirror turns things.
- **`views.portal(a, b)`** shows, in `a`, what is in front of `b`: stepping
  through `a` would bring you out of `b`'s front, turned round. The two panes
  should be the same size, and a pair both ways is two calls.
- Each returns an object with the `Texture`, the `Camera` and the `Picture`
  it made -- their `Resolution`, `UpdateInterval` and `Quality` are yours to
  change -- and `Destroy()`, which takes them all away.

**The pictures are exact.** The camera looks straight through the glass, so
the glass is a rectangle in its picture; the `ImageLabel`'s `ImageRectOffset`
and `ImageRectSize` cut that rectangle out, and a negative width turns it round
for a mirror. The reflection lines up with the room from wherever it is seen.

**`Camera.ClipPlane`** is what keeps the wall behind the glass out of the
picture: with `ClipPlaneEnabled`, the camera draws nothing behind the plane
through the `ClipPlane`'s position, facing its `LookVector`. The camera must be
behind the plane, as a mirror's and a portal's always are.

**A view draws the world, not the UI in it**: a `SurfaceGui` or a
`BillboardGui` -- a sign, a monitor, another mirror's picture -- is not in a
camera texture's picture. A mirror seen in another mirror is its bare glass,
and a feed of the security office shows its monitor wall dark.

## A game inside the game: `SubWorld`

An arcade cabinet you can play, a computer in the game running a game of its
own, a snow globe with its own weather: a `SubWorld` runs a scene beside this
one, in a world of its own, and draws it into a `view://` name.

```luau
--!strict
local cabinet = Instance.new("SubWorld")
cabinet.Scene = "scenes/arcade.scene.json"   -- the scene it runs
cabinet.ViewName = "arcade"                   -- draws into view://arcade
cabinet.Parent = workspace.Arcade
cabinet:Load()

cabinet:Send("coin")                          -- into its world...
cabinet:SetInputState("Move", 1)              -- ...its input...
cabinet.Received:Connect(function(kind: string, score: number)
    print(kind, score)                        -- ...and out of it
end)
```

And inside, in the scene's own code:

```luau
--!strict
local SceneService = game:GetService("SceneService")

SceneService.HostMessageReceived:Connect(function(kind: string)
    if kind == "coin" then
        -- start a game
    end
end)
SceneService:SendToHost("score", 12)
```

- **A world of its own, completely**: its own instances, scripts, physics,
  navigation and clock. A script there cannot find an instance here, nor one
  here an instance there. What crosses is what `Send` and `SendToHost` carry
  -- the plain values a `RemoteEvent` takes, never an instance -- delivered on
  the other side's next tick.
- **It runs its scene's own code**, `src/scenes/<scene>/server/` and
  `client/` and the scripts saved in the scene, as a game played alone. Not the
  project's `src/client`, `src/server` or `src/shared`: those are the game that
  runs it. Inside, `SceneService:IsSubWorld()` is true.
- **Its input is what you give it.** The keyboard, the mouse and the pads are
  this world's. `SetInputState(action, value)` holds its `InputAction` named
  `action` at `value` -- true or false, a number, a `Vector2` or a `vector` --
  until it is set again, so the game running it decides what reaches it and
  when.
- **It ticks with this world**: one tick of its own after each of this
  world's, at the same step, so a replay of this world replays it too. Its
  world hash is its own, and nothing in it moves this world's.
- `Load()` returns at once: the world boots after the tick, `Loaded` fires
  when its scripts have started, and `IsLoaded()` says so until `Unload()` (or
  `Destroy()`). `Running = false` pauses it and keeps its picture.
- **One level deep**: a sub-world cannot load a `SubWorld` of its own, and
  `[render] max_sub_worlds` (2 by default) are the most that run at once -- a
  third `Load()` raises, naming the limit.

**Its picture is its world**, from its own `workspace.CurrentCamera`, drawn
with `Resolution`, `UpdateInterval` and `Quality` as a `CameraTexture` is and
under the same budget. As with any view, the UI in it is not in the picture --
a sub-world shows its score to the game running it, which writes it where it
likes -- and neither, in this release, are its own camera textures or
viewport frames. Its sounds are silent: the one audio device is the game's.

**It does not replicate.** In a match, each machine that loads one runs its
own, which is what an arcade cabinet each player plays alone is.

## What it costs

A camera texture is the world drawn again, from another place. So the budget
is a setting, in `project.toml`:

```toml
[render]
max_views_per_frame = 4      # at most this many drawn in one frame
max_view_resolution = 1024   # the largest side a view may have
```

When more are due than the budget allows, the ones still black go first and
then the oldest picture, so a wall of twelve monitors takes turns instead of
costing twelve views. `UpdateInterval` is the other lever: six feeds at 2 are
three views a frame. The F3 overlay -- and the editor's Stats panel -- list
every view, its size, what it cost and how many frames ago it was drawn. A
`ViewportFrame` counts against the same budget whenever it is redrawn, and a
`SubWorld` whenever its picture is.

A `SubWorld` costs more than its picture: it is a second game -- a VM, a
physics system and a tick -- whether or not it is on screen, which is why
there is a limit on how many run at once:

```toml
[render]
max_sub_worlds = 2           # at most this many running at once
```

## In a match

A `CameraTexture` does not replicate, because a `Camera` does not: a
replica's view is its own. A feed is set up by the machine that draws it, so
in a match a client script makes its cameras and their textures. A dedicated
server draws nothing.

## A worked example

`examples/28-arcade` is an arcade: two cabinets, each a `SubWorld` playing a
small game by itself until somebody puts a coin in. Press E at the left one
and the hall sends it a coin, passes the arrow keys on as its `Move` action,
and writes the score it sends back on the cabinet's marquee.

`examples/27-mirrors-and-portals` is a gallery with a mirror and a door onto
a garden room across the map, seen by a camera walking past.

`examples/26-security-cameras` is a security office at night: six cameras in
six rooms, a `SurfaceGui` monitor wall showing all six, and a tablet that shows
one full size -- the chosen feed every frame at 640 by 360, the rest every
other frame at 320 by 180. In the corner, what the guard carries turns in three
`ViewportFrame` slots.
