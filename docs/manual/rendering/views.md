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
frame's picture. A camera that sees its own texture sees the previous one:
there is no recursion to worry about. Each camera keeps **its own exposure**:
a camera in a dark room opens up, the way a real one does, whatever the main
view is looking at.

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
every view, its size, what it cost and how many frames ago it was drawn.

## In a match

A `CameraTexture` does not replicate, because a `Camera` does not: a
replica's view is its own. A feed is set up by the machine that draws it, so
in a match a client script makes its cameras and their textures. A dedicated
server draws nothing.

## A worked example

`examples/26-security-cameras` is a security office at night: six cameras in
six rooms, a `SurfaceGui` monitor wall showing all six, and a tablet that shows
one full size -- the chosen feed every frame at 640 by 360, the rest every
other frame at 320 by 180.
