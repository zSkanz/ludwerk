--!strict
--- `GraphicsService` (ADR 0147) as a script meets it: every setting's bounds,
--- whose word a value is, what a level and a group are, and the player's file.
--- How the layers resolve is `engine/scene/tests/graphics_model_tests.cpp`'s.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local Graphics = game:GetService("GraphicsService")

--- Every case starts from the project's defaults.
local function fresh()
    Graphics:ResetToDefaults()
end

describe("GraphicsService", function()
    it("starts at the machine's level, in a window, waiting for the display", function()
        fresh()
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.High)
        expect(Graphics.RenderScale):ToBe(1)
        expect(Graphics.ShadowQuality):ToBe(Enum.ShadowQuality.High)
        expect(Graphics.AntiAliasing):ToBe(Enum.AntiAliasingMode.SMAA)
        expect(Graphics.Upscaling):ToBe(Enum.UpscalingMode.None)
        expect(Graphics.Sharpness):ToBe(0.2)
        -- No level turns frame generation on (ADR 0165).
        expect(Graphics.FrameGeneration):ToBe(false)
        expect(Graphics.Bloom):ToBe(true)
        expect(Graphics.WindowMode):ToBe(Enum.WindowMode.Windowed)
        expect(Graphics.Resolution):ToBe(Vector2.new(0, 0))
        expect(Graphics.Monitor):ToBe(0)
        expect(Graphics.VSync):ToBe(true)
        expect(Graphics.MaxFrameRate):ToBe(0)
        expect(Graphics.BackgroundFrameRate):ToBe(10)
    end)

    it("clamps a number into its range and refuses what is not that kind", function()
        fresh()
        Graphics.ShadowDistance = 5000
        expect(Graphics.ShadowDistance):ToBe(1000)
        Graphics.RenderScale = 0.1
        expect(math.abs(Graphics.RenderScale - 1 / 3) < 1e-4):ToBe(true)
        -- FSR 2 is a way to upscale like the others (ADR 0164).
        Graphics.Upscaling = Enum.UpscalingMode.FSR2
        expect(Graphics.Upscaling):ToBe(Enum.UpscalingMode.FSR2)
        -- A frame made between every two drawn is a game's to ask for
        -- (ADR 0165), and a flag like the others.
        Graphics.FrameGeneration = true
        expect(Graphics.FrameGeneration):ToBe(true)
        expect(function()
            (Graphics :: any).FrameGeneration = 1
        end):ToThrow()
        Graphics.MaxFrameRate = 144
        expect(Graphics.MaxFrameRate):ToBe(144)
        Graphics.FoliageDensity = 2
        expect(Graphics.FoliageDensity):ToBe(1)
        -- How small a rig may be and still be posed every tick: 1 until a
        -- game says otherwise, and held to 0.25 to 8.
        expect(Graphics.AnimationDetail):ToBe(1)
        Graphics.AnimationDetail = 4
        expect(Graphics.AnimationDetail):ToBe(4)
        Graphics.AnimationDetail = 100
        expect(Graphics.AnimationDetail):ToBe(8)
        Graphics.AnimationDetail = 1

        expect(function()
            Graphics.MaxFrameRate = 59.5
        end):ToThrow()
        expect(function()
            (Graphics :: any).VSync = 1
        end):ToThrow()
        expect(function()
            (Graphics :: any).WindowMode = Enum.ShadowQuality.Low
        end):ToThrow()
        expect(function()
            (Graphics :: any).ShadowDistance = "far"
        end):ToThrow()
        expect(Graphics.MaxFrameRate):ToBe(144)
    end)

    it("takes a resolution as whole pixels", function()
        fresh()
        Graphics.Resolution = Vector2.new(1920.4, 1080.9)
        expect(Graphics.Resolution):ToBe(Vector2.new(1920, 1080))
        expect(function()
            Graphics.Resolution = Vector2.new(-1, 100)
        end):ToThrow()
    end)

    it("says whose word a value is", function()
        fresh()
        expect(Graphics:GetSource("MaxFrameRate")):ToBe(Enum.SettingSource.Engine)
        expect(Graphics:GetSource("ShadowDistance")):ToBe(Enum.SettingSource.Preset)
        Graphics.MaxFrameRate = 60
        Graphics.ShadowDistance = 200
        expect(Graphics:GetSource("MaxFrameRate")):ToBe(Enum.SettingSource.Script)
        expect(Graphics:GetSource("ShadowDistance")):ToBe(Enum.SettingSource.Script)
        expect(Graphics:GetSource("Resolution")):ToBe(Enum.SettingSource.Engine)
        expect(function()
            Graphics:GetSource("Vsync")
        end):ToThrow()
    end)

    it("says which settings something draws by yet", function()
        expect(Graphics:IsApplied("Bloom")):ToBe(true)
        expect(Graphics:IsApplied("VSync")):ToBe(true)
        expect(Graphics:IsApplied("Brightness")):ToBe(false)
        expect(Graphics:IsApplied("TextureQuality")):ToBe(false)
    end)
end)

describe("a quality level", function()
    it("is every quality setting at once, and reads Custom when one is not its", function()
        fresh()
        Graphics.QualityLevel = Enum.GraphicsQuality.Low
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Low)
        expect(Graphics.Bloom):ToBe(false)
        expect(Graphics.AmbientOcclusion):ToBe(false)
        expect(Graphics.RenderScale):ToBe(0.75)
        expect(Graphics.ShadowQuality):ToBe(Enum.ShadowQuality.Low)
        expect(Graphics.ShadowCascades):ToBe(2)
        expect(Graphics.ShadowDistance):ToBe(70)

        Graphics.Bloom = true
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Custom)
        -- A display setting is not part of a level.
        Graphics.Bloom = false
        Graphics.VSync = false
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Low)
    end)

    it("is applied over whatever was said before, and leaves the display alone", function()
        fresh()
        Graphics.ShadowDistance = 333
        Graphics.Bloom = false
        Graphics.MaxFrameRate = 90
        Graphics:ApplyPreset(Enum.GraphicsQuality.Ultra)
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Ultra)
        expect(Graphics.ShadowDistance):ToBe(160)
        expect(Graphics.Bloom):ToBe(true)
        expect(Graphics.MaxFrameRate):ToBe(90)
    end)

    it("cannot be Custom, which is a reading and not a level", function()
        fresh()
        expect(function()
            Graphics.QualityLevel = Enum.GraphicsQuality.Custom
        end):ToThrow()
        expect(function()
            Graphics:ApplyPreset(Enum.GraphicsQuality.Custom)
        end):ToThrow()
        Graphics:ApplyPreset(Enum.GraphicsQuality.Auto)
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Auto)
    end)

    it("a shadow quality is its resolution and its cascades", function()
        fresh()
        Graphics.ShadowQuality = Enum.ShadowQuality.Medium
        expect(Graphics.ShadowResolution):ToBe(1024)
        expect(Graphics.ShadowCascades):ToBe(3)
        Graphics.ShadowQuality = Enum.ShadowQuality.Off
        expect(Graphics.ShadowCascades):ToBe(0)
    end)
end)

describe("a group", function()
    it("is set to a level at once and reads the level it is at", function()
        fresh()
        expect(Graphics:GetGroupLevel(Enum.GraphicsGroup.Shadows)):ToBe(Enum.GraphicsLevel.High)
        Graphics:SetGroupLevel(Enum.GraphicsGroup.Shadows, Enum.GraphicsLevel.Low)
        expect(Graphics:GetGroupLevel(Enum.GraphicsGroup.Shadows)):ToBe(Enum.GraphicsLevel.Low)
        expect(Graphics.ShadowDistance):ToBe(70)
        expect(Graphics.ContactShadows):ToBe(false)
        -- The rest is where it was.
        expect(Graphics:GetGroupLevel(Enum.GraphicsGroup.PostProcessing)):ToBe(Enum.GraphicsLevel.High)
        expect(Graphics.Bloom):ToBe(true)
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Custom)

        Graphics.ShadowDistance = 85
        expect(Graphics:GetGroupLevel(Enum.GraphicsGroup.Shadows)):ToBe(nil)

        Graphics:SetGroupLevel(Enum.GraphicsGroup.Foliage, Enum.GraphicsLevel.Low)
        expect(Graphics.FoliageDensity):ToBe(0.5)
        Graphics:SetGroupLevel(Enum.GraphicsGroup.Shadows, Enum.GraphicsLevel.Cinematic)
        expect(Graphics.ShadowDistance):ToBe(160)
    end)
end)

describe("the display", function()
    it("can be asked what it is", function()
        local monitors = Graphics:GetMonitors()
        expect(type(monitors)):ToBe("table")
        for _, monitor in monitors do
            expect(type(monitor.Name)):ToBe("string")
            expect(monitor.Size.X >= 0):ToBe(true)
            expect(monitor.RefreshRate >= 0):ToBe(true)
        end
        expect(type(Graphics:GetSupportedResolutions())):ToBe("table")
        expect(Graphics:GetRefreshRate() >= 0):ToBe(true)
    end)
end)

describe("the player's choices", function()
    it("are kept by SaveAsync, come back with LoadAsync, and go with ResetToDefaults", function()
        fresh()
        Graphics.Bloom = false
        Graphics.MaxFrameRate = 120
        expect(Graphics:GetSource("Bloom")):ToBe(Enum.SettingSource.Script)
        expect(Graphics:SaveAsync()):ToBe(true)
        -- Saved: the player's now, and nothing a script has yet to save.
        expect(Graphics:GetSource("Bloom")):ToBe(Enum.SettingSource.Player)
        expect(Graphics:GetSource("MaxFrameRate")):ToBe(Enum.SettingSource.Player)
        expect(Graphics.Bloom):ToBe(false)

        -- Changed and not saved: a revert reads the file again.
        Graphics.MaxFrameRate = 30
        Graphics.Bloom = true
        expect(Graphics:LoadAsync()):ToBe(true)
        expect(Graphics.MaxFrameRate):ToBe(120)
        expect(Graphics.Bloom):ToBe(false)
        expect(Graphics:GetSource("MaxFrameRate")):ToBe(Enum.SettingSource.Player)

        -- The defaults: neither a script's word nor the player's stands.
        Graphics:ResetToDefaults()
        expect(Graphics.Bloom):ToBe(true)
        expect(Graphics.MaxFrameRate):ToBe(0)
        expect(Graphics:GetSource("Bloom")):ToBe(Enum.SettingSource.Preset)
        -- And saved, so the file holds nothing either.
        expect(Graphics:SaveAsync()):ToBe(true)
        expect(Graphics:LoadAsync()):ToBe(true)
        expect(Graphics.Bloom):ToBe(true)
    end)
end)

describe("QualityChanged", function()
    it("fires when a quality setting changes, once a frame, and not for the display's", function()
        fresh()
        task.wait()
        task.wait()
        local fired = 0
        local connection = Graphics.QualityChanged:Connect(function()
            fired += 1
        end)
        Graphics.Bloom = false
        Graphics.ShadowDistance = 90
        Graphics.DepthOfField = false
        task.wait()
        task.wait()
        task.wait()
        expect(fired):ToBe(1)

        Graphics.VSync = false
        Graphics.MaxFrameRate = 75
        task.wait()
        task.wait()
        task.wait()
        expect(fired):ToBe(1)
        connection:Disconnect()
        fresh()
    end)
end)
