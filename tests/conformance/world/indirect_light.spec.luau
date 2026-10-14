--!strict
--- `ReflectionProbe` and `IrradianceVolume` (ADRs 0202, 0203): what a room
--- reflects, and the light it gives back. What each is made with, what it
--- takes and what it refuses -- the light itself is a picture, each machine's
--- own, and is held where pictures are (the renderer's and the app's tests).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local GraphicsService = game:GetService("GraphicsService")

describe("ReflectionProbe", function()
    it("is made applying, in the middle of its box, corrected for the box", function()
        local probe = Instance.new("ReflectionProbe")
        expect(probe.Enabled):ToBe(true)
        expect(probe.Offset):ToBe(vector.zero)
        expect(probe.Blend):ToBe(1)
        expect(probe.Intensity):ToBe(1)
        expect(probe.Priority):ToBe(0)
        expect(probe.BoxProjection):ToBe(true)
        expect(probe.Dynamic):ToBe(false)
        probe:Destroy()
    end)

    it("takes what it is set, and a priority as a whole number", function()
        local room = Instance.new("Part")
        local probe = Instance.new("ReflectionProbe")
        probe.Parent = room
        probe.Offset = vector.create(0, 1, -2)
        expect(probe.Offset):ToBe(vector.create(0, 1, -2))
        probe.Blend = 0
        expect(probe.Blend):ToBe(0)
        probe.Intensity = 2.5
        expect(probe.Intensity):ToBe(2.5)
        probe.Priority = 3.6
        expect(probe.Priority):ToBe(4)
        probe.Priority = -2
        expect(probe.Priority):ToBe(-2)
        probe.BoxProjection = false
        probe.Dynamic = true
        probe.Enabled = false
        expect(probe.BoxProjection):ToBe(false)
        expect(probe.Dynamic):ToBe(true)
        expect(probe.Enabled):ToBe(false)
        room:Destroy()
    end)

    it("refuses a blend or an intensity below nought", function()
        local probe = Instance.new("ReflectionProbe")
        expect(function()
            probe.Blend = -0.5
        end):ToThrow()
        expect(function()
            probe.Intensity = -1
        end):ToThrow()
        expect(probe.Blend):ToBe(1)
        expect(probe.Intensity):ToBe(1)
        probe:Destroy()
    end)

    it("can be asked for again, whether or not anything takes its pictures", function()
        local room = Instance.new("Part")
        local probe = Instance.new("ReflectionProbe")
        probe.Parent = room
        probe:Refresh()
        probe:Refresh()
        expect(probe.Enabled):ToBe(true)
        room:Destroy()
    end)

    it("is copied with what it was set", function()
        local room = Instance.new("Part")
        local probe = Instance.new("ReflectionProbe")
        probe.Blend = 0.25
        probe.Priority = 7
        probe.Dynamic = true
        probe.Parent = room
        local copy = room:Clone()
        local copied = copy:FindFirstChildOfClass("ReflectionProbe") :: ReflectionProbe
        expect(copied.Blend):ToBe(0.25)
        expect(copied.Priority):ToBe(7)
        expect(copied.Dynamic):ToBe(true)
        copy:Destroy()
        room:Destroy()
    end)
end)

describe("IrradianceVolume", function()
    it("is made applying, two metres between its points, one bounce", function()
        local volume = Instance.new("IrradianceVolume")
        expect(volume.Enabled):ToBe(true)
        expect(volume.Spacing):ToBe(2)
        expect(volume.Blend):ToBe(1)
        expect(volume.Intensity):ToBe(1)
        expect(volume.Bounces):ToBe(1)
        expect(volume.Dynamic):ToBe(false)
        volume:Destroy()
    end)

    it("takes what it is set", function()
        local room = Instance.new("Part")
        local volume = Instance.new("IrradianceVolume")
        volume.Parent = room
        volume.Spacing = 0.5
        expect(volume.Spacing):ToBe(0.5)
        volume.Bounces = 3
        expect(volume.Bounces):ToBe(3)
        volume.Blend = 0
        volume.Intensity = 0.5
        volume.Dynamic = true
        expect(volume.Blend):ToBe(0)
        expect(volume.Intensity):ToBe(0.5)
        expect(volume.Dynamic):ToBe(true)
        room:Destroy()
    end)

    it("refuses a spacing finer than half a metre, and bounces that are not one, two or three", function()
        local volume = Instance.new("IrradianceVolume")
        expect(function()
            volume.Spacing = 0.25
        end):ToThrow()
        expect(function()
            volume.Bounces = 0
        end):ToThrow()
        expect(function()
            volume.Bounces = 4
        end):ToThrow()
        expect(function()
            volume.Bounces = 1.5
        end):ToThrow()
        expect(volume.Spacing):ToBe(2)
        expect(volume.Bounces):ToBe(1)
        volume:Destroy()
    end)

    it("says how much of its light is worked out, and all of it where none is", function()
        -- A run with nothing to draw with works no light out, and a loading
        -- screen that waited for it would wait for ever: it says one.
        local room = Instance.new("Part")
        local volume = Instance.new("IrradianceVolume")
        volume.Parent = room
        local progress = volume:GetProgress()
        expect(progress >= 0 and progress <= 1):ToBe(true)
        volume:Refresh()
        expect(volume:GetProgress() <= 1):ToBe(true)
        room:Destroy()
    end)

    it("has a tint beside its intensity, and a mode: everything, or what bounces alone", function()
        local room = Instance.new("Part")
        local volume = Instance.new("IrradianceVolume")
        volume.Parent = room
        expect(volume.Mode):ToBe(Enum.IrradianceMode.Full)
        expect(volume.Tint):ToBe(Color3.new(1, 1, 1))
        volume.Mode = Enum.IrradianceMode.BounceOnly
        volume.Tint = Color3.new(1, 0.5, 0.25)
        expect(volume.Mode):ToBe(Enum.IrradianceMode.BounceOnly)
        expect(volume.Tint.G):ToBeCloseTo(0.5, 0.001)
        local copy = room:Clone()
        local copied = copy:FindFirstChildOfClass("IrradianceVolume") :: IrradianceVolume
        expect(copied.Mode):ToBe(Enum.IrradianceMode.BounceOnly)
        expect(copied.Tint.B):ToBeCloseTo(0.25, 0.001)
        copy:Destroy()
        room:Destroy()
    end)

    it("is ready where nothing works it out, before and after it is asked for again", function()
        -- This run draws nothing: no ray will ever be cast, so a script that
        -- waited for a volume or a probe would wait for ever. Each says it is
        -- ready, and `Refresh` does not unsay it.
        local room = Instance.new("Part")
        room.Anchored = true
        room.Parent = workspace
        local volume = Instance.new("IrradianceVolume")
        volume.Parent = room
        local probe = Instance.new("ReflectionProbe")
        probe.Parent = room
        expect(volume:IsReady()):ToBe(true)
        expect(probe:IsReady()):ToBe(true)
        expect(volume:GetProgress()):ToBe(1)
        volume:Refresh()
        probe:Refresh()
        expect(volume:IsReady()):ToBe(true)
        expect(probe:IsReady()):ToBe(true)
        expect(volume:GetProgress()):ToBe(1)
        -- And the events are there to be waited on.
        local heard = 0
        local connection = volume.Completed:Connect(function()
            heard += 1
        end)
        connection:Disconnect()
        expect(heard):ToBe(0)
        room:Destroy()
    end)
end)

describe("GraphicsService.GlobalIlluminationBudget", function()
    it("is the milliseconds of a frame bounced light may take, a quarter to sixteen", function()
        expect(GraphicsService:IsApplied("GlobalIlluminationBudget")):ToBe(true)
        local before = GraphicsService.GlobalIlluminationBudget
        expect(before):ToBeCloseTo(1.5, 0.001)
        GraphicsService.GlobalIlluminationBudget = 4
        expect(GraphicsService.GlobalIlluminationBudget):ToBe(4)
        -- Out of its range it is the nearest end, as every number here is.
        GraphicsService.GlobalIlluminationBudget = 0
        expect(GraphicsService.GlobalIlluminationBudget):ToBe(0.25)
        GraphicsService.GlobalIlluminationBudget = 100
        expect(GraphicsService.GlobalIlluminationBudget):ToBe(16)
        GraphicsService.GlobalIlluminationBudget = before
    end)
end)

describe("GraphicsService.Reflections and GlobalIllumination", function()
    it("are levels a game reads and sets, and are applied", function()
        expect(GraphicsService:IsApplied("Reflections")):ToBe(true)
        expect(GraphicsService:IsApplied("GlobalIllumination")):ToBe(true)
        local before = GraphicsService.Reflections
        GraphicsService.Reflections = Enum.GraphicsLevel.Low
        expect(GraphicsService.Reflections):ToBe(Enum.GraphicsLevel.Low)
        GraphicsService.Reflections = before
        local light = GraphicsService.GlobalIllumination
        GraphicsService.GlobalIllumination = Enum.GraphicsLevel.Ultra
        expect(GraphicsService.GlobalIllumination):ToBe(Enum.GraphicsLevel.Ultra)
        GraphicsService.GlobalIllumination = light
    end)

    it("and so are the two that read the screen (ADR 0209)", function()
        expect(GraphicsService:IsApplied("ScreenSpaceReflections")):ToBe(true)
        expect(GraphicsService:IsApplied("ScreenSpaceBounce")):ToBe(true)
        local reflections = GraphicsService.ScreenSpaceReflections
        GraphicsService.ScreenSpaceReflections = Enum.GraphicsLevel.Ultra
        expect(GraphicsService.ScreenSpaceReflections):ToBe(Enum.GraphicsLevel.Ultra)
        GraphicsService.ScreenSpaceReflections = Enum.GraphicsLevel.Low
        expect(GraphicsService.ScreenSpaceReflections):ToBe(Enum.GraphicsLevel.Low)
        GraphicsService.ScreenSpaceReflections = reflections
        local bounce = GraphicsService.ScreenSpaceBounce
        GraphicsService.ScreenSpaceBounce = Enum.GraphicsLevel.High
        expect(GraphicsService.ScreenSpaceBounce):ToBe(Enum.GraphicsLevel.High)
        GraphicsService.ScreenSpaceBounce = bounce
    end)
end)
