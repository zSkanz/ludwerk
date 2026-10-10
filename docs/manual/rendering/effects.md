# Highlights, beams and trails

Three things every game reaches for, each one instance: an outline on what is
selected or on an ally behind a wall, a band of light between two points, and
the ribbon something leaves behind as it moves.

**All three are pictures, not the world.** Nothing collides with them, a ray
does not find them, and they are drawn on the frame rather than simulated on
the tick -- so they cost nothing a replay or a second machine has to agree on,
and a dedicated server draws none.

## A highlight

A `Highlight` marks a part, or a model and every part in it, with a tint over
its shape and a line round its edge. Parent it to what it marks, or name
something else with `Adornee`.

```luau
--!strict
local function mark(target: Instance, color: Color3): Highlight
    local highlight = Instance.new("Highlight")
    highlight.FillColor = color
    highlight.FillTransparency = 0.7
    highlight.OutlineColor = color
    highlight.Parent = target
    return highlight
end
```

| Property | What it is |
|---|---|
| `Adornee` | What is marked: a part or a model. Nothing means the highlight's parent. |
| `FillColor`, `FillTransparency` | The tint over the whole shape; a transparency of 1 leaves only the line. |
| `OutlineColor`, `OutlineTransparency` | The line round the shape's edge; 1 leaves only the tint. |
| `DepthMode` | `AlwaysOnTop` shows through whatever is in front -- an ally behind a wall. `Occluded` marks only where the shape itself is seen. |
| `Enabled` | Whether it is drawn. |

**The colours are the ones you give.** A highlight is drawn over the finished
picture, after the light, the exposure and the tone curve, so
`Color3.fromRGB(255, 220, 0)` is that yellow at noon and at midnight.

**There is a budget.** Each highlight is a pass of its own, so a frame draws at
most `max_highlights` of them -- 32 unless `project.toml` says otherwise:

```toml
[render]
max_highlights = 64
```

Past the budget the nearest win, and F3 says how many were left out. A
highlight nested inside another -- a sword in a marked character's hand, with
a highlight of its own -- takes its own.

## A beam

A `Beam` is a band between two [attachments](manual:physics/joints): a laser,
a tractor beam, a rope of light. It follows them wherever they go.

```luau
--!strict
local function laser(from: Attachment, to: Attachment): Beam
    local beam = Instance.new("Beam")
    beam.Attachment0 = from
    beam.Attachment1 = to
    beam.Color = ColorSequence.new(Color3.new(1, 0.1, 0.1))
    beam.Width0 = 0.2
    beam.Width1 = 0.2
    beam.LightEmission = 1 -- it adds light instead of covering what is behind
    beam.LightInfluence = 0 -- and is not dimmed by the hour
    beam.Parent = from
    return beam
end
```

- **`Color` and `Transparency`** are sequences along its length, from
  `Attachment0` at 0 to `Attachment1` at 1.
- **`Width0` and `Width1`** are its width at each end, in metres.
- **`CurveSize0` and `CurveSize1`** bend it: the beam leaves `Attachment0`
  along that attachment's own X for that many metres, and arrives at
  `Attachment1` along its X. Both zero is a straight line; `Segments` is how
  many straight pieces a curve is drawn as.
- **`FaceCamera`** turns the band about its own length to face whoever is
  looking, which is what a ray of light wants. Off, it is a flat ribbon lying
  in the plane of the attachments' Y axes, with two sides.
- **`LightEmission`** blends between covering what is behind it (0) and adding
  to it (1); **`LightInfluence`** is how much the scene's light colours it.
- **`ZOffset`** draws it that many metres nearer the camera, so a beam lying
  along a wall is not cut by the wall.

### A texture along it

`Texture` is an image whose U runs along the beam and V across it.
`TextureMode` says how it is laid:

| `TextureMode` | The texture | `TextureSpeed` |
|---|---|---|
| `Stretch` | once over the whole length | repeats a second |
| `Wrap` | repeats every `TextureLength` metres | metres a second |
| `Static` | repeats every `TextureLength` metres | does not run |

A positive speed runs from `Attachment0` towards `Attachment1`: lightning that
crawls, a conveyor of energy.

## A trail

A `Trail` is the ribbon two attachments leave as they move: a sword's swing,
a tyre's mark, the wake of something fast. The two attachments are its two
edges; put them on the thing that moves.

```luau
--!strict
local function swing(blade: BasePart): Trail
    local hilt = Instance.new("Attachment")
    hilt.CFrame = CFrame.new(0, -1, 0)
    hilt.Parent = blade
    local tip = Instance.new("Attachment")
    tip.CFrame = CFrame.new(0, 1, 0)
    tip.Parent = blade

    local trail = Instance.new("Trail")
    trail.Attachment0 = hilt
    trail.Attachment1 = tip
    trail.Lifetime = 0.25
    trail.Transparency = NumberSequence.new(0, 1) -- solid at the blade, gone at the end
    trail.LightEmission = 1
    trail.Parent = blade
    return trail
end
```

- **`Lifetime`** is how long a piece lasts, in seconds. A trail is as long as
  its ends went in that time: twice the speed, twice the trail.
- **`Color`, `Transparency` and `WidthScale`** are sequences over a piece's
  life -- new at 0, `Lifetime` old at 1. `WidthScale` is a fraction of the
  distance between the two attachments.
- **`MinLength`** is how far the ends must move before the trail gains a
  piece; smaller is smoother and costs more. **`MaxLength`** cuts it shorter
  than `Lifetime` would; zero is no limit.
- **`Enabled = false`** stops it growing; what is there fades out over its
  lifetime. **`Clear()`** drops all of it at once, which is what something
  that teleports calls so the ribbon does not stretch from where it was.
- **`FaceCamera`** turns the ribbon to face the camera about its middle line.
  Off -- the default -- it is the surface the two attachments swept.
- **`TextureMode.Static`** fixes the picture where it was laid, as a tyre mark
  stays on the road; `Wrap` lays it from the moving end, so it slides along.

## What they cost, and where to see it

A beam is one quad a segment and a trail one a piece, all in one buffer drawn
in the particles' pass. F3's **Effects** section counts the highlights drawn
and left out, the beams, the trails, the quads they are and the particles
alive.

## In a match

A highlight, a beam or a trail a server-side script makes is on every machine,
with every property authored on it: a beam's and a trail's sequences, the two
attachments they run between, what a highlight marks. A change to one is sent
when it is made, and so is a trail's `Clear`. **What each draws is that
machine's own**: a trail's ribbon is laid from where that machine drew its
ends, so two players do not see the same pieces, only the same trail. One made
by a client's own script is drawn on that client alone
([What a spawned thing carries](manual:guides/multiplayer)).

## Where to look next

- [Particles and decals](manual:rendering/particles-and-decals)
- [`Highlight`](api:Highlight) · [`Beam`](api:Beam) · [`Trail`](api:Trail)
