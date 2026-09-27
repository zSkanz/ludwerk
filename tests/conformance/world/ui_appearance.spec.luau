--!strict
--- `UIGradient` and `UIStroke`, from api-design.md §2.2 and ADR 0110: their
--- defaults, the enums that describe them, where they may be parented, and
--- the sequences as attributes.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("UIGradient", function()
    it("starts as a gradient that changes nothing", function()
        local gradient = Instance.new("UIGradient")
        expect(gradient.Color):ToBe(ColorSequence.new(Color3.new(1, 1, 1)))
        expect(gradient.Transparency):ToBe(NumberSequence.new(0))
        expect(gradient.Offset):ToBe(Vector2.new(0, 0))
        expect(gradient.Rotation):ToBe(0)
        expect(gradient.Enabled):ToBe(true)
        expect(gradient.Type):ToBe(Enum.GradientType.Linear)
        expect(gradient.TileMode):ToBe(Enum.GradientTileMode.Clamp)
        expect(gradient.Scale):ToBe(1)
        gradient:Destroy()
    end)

    it("takes a new sequence, shape and tiling", function()
        local gradient = Instance.new("UIGradient")
        gradient.Color = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))
        gradient.Transparency = NumberSequence.new(0, 1)
        gradient.Type = Enum.GradientType.Conical
        gradient.TileMode = Enum.GradientTileMode.Mirror
        gradient.Scale = 0.25
        expect(gradient.Color.Keypoints[2].Value):ToBe(Color3.new(0, 0, 1))
        expect(gradient.Transparency.Keypoints[2].Value):ToBe(1)
        expect(gradient.Type):ToBe(Enum.GradientType.Conical)
        expect(gradient.TileMode):ToBe(Enum.GradientTileMode.Mirror)
        expect(gradient.Scale):ToBe(0.25)
        gradient:Destroy()
    end)

    it("refuses a scale that is not above zero, and a colour where a sequence goes", function()
        local gradient = Instance.new("UIGradient")
        expect(function()
            gradient.Scale = 0
        end):ToThrow()
        expect(function()
            (gradient :: any).Color = Color3.new(1, 0, 0)
        end):ToThrow()
        gradient:Destroy()
    end)

    it("may colour a stroke", function()
        local frame = Instance.new("Frame")
        local stroke = Instance.new("UIStroke")
        stroke.Parent = frame
        local gradient = Instance.new("UIGradient")
        gradient.Parent = stroke
        expect(gradient.Parent):ToBe(stroke)
        frame:Destroy()
    end)

    it("names its shapes and tilings as the specification lists them", function()
        expect(Enum.GradientType.Linear.Value):ToBe(0)
        expect(Enum.GradientType.Radial.Value):ToBe(1)
        expect(Enum.GradientType.Conical.Value):ToBe(2)
        expect(Enum.GradientTileMode.Clamp.Value):ToBe(0)
        expect(Enum.GradientTileMode.Repeat.Value):ToBe(1)
        expect(Enum.GradientTileMode.Mirror.Value):ToBe(2)
    end)
end)

describe("UIStroke", function()
    it("starts as a one-pixel black outline outside the border", function()
        local stroke = Instance.new("UIStroke")
        expect(stroke.Color):ToBe(Color3.new(0, 0, 0))
        expect(stroke.Thickness):ToBe(1)
        expect(stroke.Transparency):ToBe(0)
        expect(stroke.Enabled):ToBe(true)
        expect(stroke.ApplyStrokeMode):ToBe(Enum.ApplyStrokeMode.Contextual)
        expect(stroke.LineJoinMode):ToBe(Enum.LineJoinMode.Round)
        expect(stroke.StrokeSizingMode):ToBe(Enum.StrokeSizingMode.FixedSize)
        expect(stroke.BorderStrokePosition):ToBe(Enum.BorderStrokePosition.Outer)
        expect(stroke.BorderOffset):ToBe(UDim.new(0, 0))
        expect(stroke.ZIndex):ToBe(1)
        stroke:Destroy()
    end)

    it("refuses a negative thickness", function()
        local stroke = Instance.new("UIStroke")
        expect(function()
            stroke.Thickness = -1
        end):ToThrow()
        stroke:Destroy()
    end)

    it("lets several strokes share one element", function()
        local frame = Instance.new("Frame")
        for index = 1, 3 do
            local stroke = Instance.new("UIStroke")
            stroke.ZIndex = index
            stroke.Parent = frame
        end
        local strokes = 0
        for _, child in frame:GetChildren() do
            if child:IsA("UIStroke") then
                strokes += 1
            end
        end
        expect(strokes):ToBe(3)
        frame:Destroy()
    end)

    it("names its modes as the specification lists them", function()
        expect(Enum.ApplyStrokeMode.Contextual.Value):ToBe(0)
        expect(Enum.ApplyStrokeMode.Border.Value):ToBe(1)
        expect(Enum.LineJoinMode.Round.Value):ToBe(0)
        expect(Enum.LineJoinMode.Bevel.Value):ToBe(1)
        expect(Enum.LineJoinMode.Miter.Value):ToBe(2)
        expect(Enum.StrokeSizingMode.FixedSize.Value):ToBe(0)
        expect(Enum.StrokeSizingMode.ScaledSize.Value):ToBe(1)
        expect(Enum.BorderStrokePosition.Outer.Value):ToBe(0)
        expect(Enum.BorderStrokePosition.Center.Value):ToBe(1)
        expect(Enum.BorderStrokePosition.Inner.Value):ToBe(2)
    end)
end)

describe("sequences as attributes", function()
    it("keeps a ColorSequence and a NumberSequence as it was given", function()
        local holder = Instance.new("Folder")
        local colors = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 1, 0))
        holder:SetAttribute("Colors", colors)
        holder:SetAttribute("Fade", NumberSequence.new(0, 1))
        expect(holder:GetAttribute("Colors")):ToBe(colors)
        expect(holder:GetAttribute("Fade")):ToBe(NumberSequence.new(0, 1))
        holder:Destroy()
    end)
end)
