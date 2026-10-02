--!strict
--- Conformance: `Highlight`, `Beam` and `Trail` (ADR 0129).
---
--- Three ways of drawing something, each an instance with properties: a mark
--- over a shape, a band between two points, the ribbon two points leave. They
--- are pictures -- nothing here can observe what is drawn, and that is the
--- point -- so what a spec can ask is what a script can: that each exists with
--- the defaults its page says, keeps what it is given, and refuses what makes
--- no sense. What they look like is `tests/screenshots/effects`'s to say, and
--- how a trail grows is `ribbons_tests.cpp`'s.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local spawned: { Instance } = {}

local function keep<T>(instance: T & Instance): T
    table.insert(spawned, instance)
    return instance
end

local function part(): Part
    local created = keep(Instance.new("Part"))
    created.Anchored = true
    created.Parent = workspace
    return created
end

local function attachmentOn(host: BasePart): Attachment
    local created = keep(Instance.new("Attachment"))
    created.Parent = host
    return created
end

afterEach(function()
    for _, instance in spawned do
        instance:Destroy()
    end
    table.clear(spawned)
end)

describe("Highlight", function()
    it("marks its parent in red and white, through walls, until told otherwise", function()
        local highlight = keep(Instance.new("Highlight"))
        expect(highlight.Adornee):ToBe(nil)
        expect(highlight.FillColor):ToBe(Color3.new(1, 0.2, 0.2))
        expect(highlight.FillTransparency):ToBe(0.5)
        expect(highlight.OutlineColor):ToBe(Color3.new(1, 1, 1))
        expect(highlight.OutlineTransparency):ToBe(0)
        expect(highlight.DepthMode):ToBe(Enum.HighlightDepthMode.AlwaysOnTop)
        expect(highlight.Enabled):ToBe(true)
    end)

    it("keeps what it is given", function()
        local target = part()
        local model = keep(Instance.new("Model"))
        model.Parent = workspace
        local highlight = keep(Instance.new("Highlight"))
        highlight.Adornee = target
        highlight.FillColor = Color3.new(0, 1, 0)
        highlight.FillTransparency = 1
        highlight.OutlineColor = Color3.new(0, 0, 1)
        highlight.OutlineTransparency = 0.25
        highlight.DepthMode = Enum.HighlightDepthMode.Occluded
        highlight.Enabled = false
        highlight.Parent = workspace
        expect(highlight.Adornee):ToBe(target)
        expect(highlight.FillColor):ToBe(Color3.new(0, 1, 0))
        expect(highlight.FillTransparency):ToBe(1)
        expect(highlight.OutlineColor):ToBe(Color3.new(0, 0, 1))
        expect(highlight.OutlineTransparency):ToBe(0.25)
        expect(highlight.DepthMode):ToBe(Enum.HighlightDepthMode.Occluded)
        expect(highlight.Enabled):ToBe(false)
        -- A model is marked as a part is, and nothing is nothing.
        highlight.Adornee = model
        expect(highlight.Adornee):ToBe(model)
        highlight.Adornee = nil
        expect(highlight.Adornee):ToBe(nil)
    end)

    it("refuses a fraction outside nought to one, and something that is not a shape", function()
        local highlight = keep(Instance.new("Highlight"))
        expect(function()
            highlight.FillTransparency = 1.5
        end):ToThrow()
        expect(function()
            highlight.OutlineTransparency = -0.1
        end):ToThrow()
        expect(function()
            (highlight :: any).Adornee = attachmentOn(part())
        end):ToThrow()
    end)

    it("forgets what it marked when that is destroyed", function()
        local target = part()
        local highlight = keep(Instance.new("Highlight"))
        highlight.Adornee = target
        target:Destroy()
        expect(highlight.Adornee):ToBe(nil)
    end)
end)

describe("Beam", function()
    it("is a white band a metre wide, straight, facing the camera", function()
        local beam = keep(Instance.new("Beam"))
        expect(beam.Attachment0):ToBe(nil)
        expect(beam.Attachment1):ToBe(nil)
        expect(#beam.Color.Keypoints):ToBe(2)
        expect(beam.Color.Keypoints[1].Value):ToBe(Color3.new(1, 1, 1))
        expect(beam.Transparency.Keypoints[1].Value):ToBe(0)
        expect(beam.Width0):ToBe(1)
        expect(beam.Width1):ToBe(1)
        expect(beam.CurveSize0):ToBe(0)
        expect(beam.CurveSize1):ToBe(0)
        expect(beam.Segments):ToBe(10)
        expect(beam.Texture):ToBe("")
        expect(beam.TextureLength):ToBe(1)
        expect(beam.TextureMode):ToBe(Enum.TextureMode.Stretch)
        expect(beam.TextureSpeed):ToBe(0)
        expect(beam.FaceCamera):ToBe(true)
        expect(beam.LightEmission):ToBe(0)
        expect(beam.LightInfluence):ToBe(1)
        expect(beam.ZOffset):ToBe(0)
        expect(beam.Enabled):ToBe(true)
    end)

    it("keeps what it is given", function()
        local from, to = attachmentOn(part()), attachmentOn(part())
        local beam = keep(Instance.new("Beam"))
        beam.Attachment0 = from
        beam.Attachment1 = to
        beam.Color = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))
        beam.Transparency = NumberSequence.new(0, 1)
        beam.Width0 = 2
        beam.Width1 = 0
        beam.CurveSize0 = -3
        beam.CurveSize1 = 4
        beam.Segments = 24
        beam.TextureLength = 2.5
        beam.TextureMode = Enum.TextureMode.Wrap
        beam.TextureSpeed = -1
        beam.FaceCamera = false
        beam.LightEmission = 1
        beam.LightInfluence = 0
        beam.ZOffset = 0.1
        beam.Enabled = false
        beam.Parent = workspace
        expect(beam.Attachment0):ToBe(from)
        expect(beam.Attachment1):ToBe(to)
        expect(beam.Color.Keypoints[2].Value):ToBe(Color3.new(0, 0, 1))
        expect(beam.Transparency.Keypoints[2].Value):ToBe(1)
        expect(beam.Width0):ToBe(2)
        expect(beam.Width1):ToBe(0)
        expect(beam.CurveSize0):ToBe(-3)
        expect(beam.CurveSize1):ToBe(4)
        expect(beam.Segments):ToBe(24)
        expect(beam.TextureLength):ToBe(2.5)
        expect(beam.TextureMode):ToBe(Enum.TextureMode.Wrap)
        expect(beam.TextureSpeed):ToBe(-1)
        expect(beam.FaceCamera):ToBe(false)
        expect(beam.LightEmission):ToBe(1)
        expect(beam.LightInfluence):ToBe(0)
        expect(beam.ZOffset):ToBeCloseTo(0.1, 1e-6)
        expect(beam.Enabled):ToBe(false)
    end)

    it("refuses an end that is not an attachment, and numbers that make no band", function()
        local beam = keep(Instance.new("Beam"))
        expect(function()
            (beam :: any).Attachment0 = part()
        end):ToThrow()
        expect(function()
            beam.Width0 = -1
        end):ToThrow()
        expect(function()
            beam.Segments = 0
        end):ToThrow()
        expect(function()
            beam.Segments = 2.5
        end):ToThrow()
        expect(function()
            beam.Segments = 65
        end):ToThrow()
        expect(function()
            beam.TextureLength = 0
        end):ToThrow()
        expect(function()
            beam.LightEmission = 2
        end):ToThrow()
    end)
end)

describe("Trail", function()
    it("lasts two seconds, as wide as its ends are apart, and is the surface they swept", function()
        local trail = keep(Instance.new("Trail"))
        expect(trail.Attachment0):ToBe(nil)
        expect(trail.Attachment1):ToBe(nil)
        expect(trail.Lifetime):ToBe(2)
        expect(trail.MinLength):ToBeCloseTo(0.1, 1e-6)
        expect(trail.MaxLength):ToBe(0)
        expect(trail.Color.Keypoints[1].Value):ToBe(Color3.new(1, 1, 1))
        expect(trail.Transparency.Keypoints[1].Value):ToBe(0)
        expect(trail.WidthScale.Keypoints[1].Value):ToBe(1)
        expect(trail.WidthScale.Keypoints[2].Value):ToBe(1)
        expect(trail.Texture):ToBe("")
        expect(trail.TextureLength):ToBe(1)
        expect(trail.TextureMode):ToBe(Enum.TextureMode.Stretch)
        expect(trail.FaceCamera):ToBe(false)
        expect(trail.LightEmission):ToBe(0)
        expect(trail.LightInfluence):ToBe(1)
        expect(trail.Enabled):ToBe(true)
    end)

    it("keeps what it is given, and can be cleared", function()
        local blade = part()
        local hilt, tip = attachmentOn(blade), attachmentOn(blade)
        local trail = keep(Instance.new("Trail"))
        trail.Attachment0 = hilt
        trail.Attachment1 = tip
        trail.Lifetime = 0.5
        trail.MinLength = 0
        trail.MaxLength = 12
        trail.WidthScale = NumberSequence.new(1, 0)
        trail.TextureMode = Enum.TextureMode.Static
        trail.FaceCamera = true
        trail.Enabled = false
        trail.Parent = blade
        expect(trail.Attachment0):ToBe(hilt)
        expect(trail.Attachment1):ToBe(tip)
        expect(trail.Lifetime):ToBe(0.5)
        expect(trail.MinLength):ToBe(0)
        expect(trail.MaxLength):ToBe(12)
        expect(trail.WidthScale.Keypoints[2].Value):ToBe(0)
        expect(trail.TextureMode):ToBe(Enum.TextureMode.Static)
        expect(trail.FaceCamera):ToBe(true)
        expect(trail.Enabled):ToBe(false)
        -- Nothing to read back: the ribbon is the renderer's. It must not
        -- raise, and the instance is what it was.
        trail:Clear()
        task.wait()
        expect(trail.Parent):ToBe(blade)
    end)

    it("refuses a lifetime of nothing and a length below nought", function()
        local trail = keep(Instance.new("Trail"))
        expect(function()
            trail.Lifetime = 0
        end):ToThrow()
        expect(function()
            trail.MinLength = -1
        end):ToThrow()
        expect(function()
            trail.MaxLength = -1
        end):ToThrow()
    end)
end)
