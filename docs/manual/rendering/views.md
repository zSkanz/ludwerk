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
- **Framed for you**: with no `CurrentCamera`, it looks at everything inside
  from the front and a little above. Put a `Camera` inside it and set
  `CurrentCamera` to choose the angle yourself.
- **Its own simple light**: `Ambient`, and one light of `LightColor` travelling
  along `LightDirection`. No sky and no shadows; with
  `BackgroundTransparency = 1`, only what is inside shows, over whatever is
  behind the frame.
- **Drawn only when something changes**: an item moved, its camera, its light,
  its size. Forty still items are forty pictures once; one turning is one
  picture a frame.

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
`ViewportFrame` counts against the same budget whenever it is redrawn.

## In a match

A `CameraTexture` does not replicate, because a `Camera` does not: a
replica's view is its own. A feed is set up by the machine that draws it, so
in a match a client script makes its cameras and their textures. A dedicated
server draws nothing.

## A worked example

`examples/27-mirrors-and-portals` is a gallery with a mirror and a door onto
a garden room across the map, seen by a camera walking past.

`examples/26-security-cameras` is a security office at night: six cameras in
six rooms, a `SurfaceGui` monitor wall showing all six, and a tablet that shows
one full size -- the chosen feed every frame at 640 by 360, the rest every
other frame at 320 by 180. In the corner, what the guard carries turns in three
`ViewportFrame` slots.
