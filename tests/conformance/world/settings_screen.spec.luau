--!strict
--- `@engine/settings` (ADR 0147, section 7): the options screen a game gets in
--- one call. What a row shows, what a step writes and when, what Apply, Revert
--- and leaving do, and whose words it is written in.

local settings = require("@engine/settings")
local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local Graphics = game:GetService("GraphicsService")
local Localization = game:GetService("LocalizationService")
local UIService = game:GetService("UIService")

--- Every case starts from the project's defaults, in English, with nothing kept.
local function fresh()
    Localization.Locale = "en"
    Graphics:ResetToDefaults()
    Graphics:SaveAsync()
end

--- A thing of the screen by the names on the way to it.
local function find(root: Instance, path: { string }): Instance
    local at = root
    for _, name in path do
        local child = at:FindFirstChild(name)
        assert(child, `nothing named {name} under {at.Name}`)
        at = child
    end
    return at
end

describe("@engine/settings", function()
    it("opens a screen over the game with a row for what is drawn by", function()
        fresh()
        local screen = settings.open()
        expect(screen.IsOpen):ToBe(true)
        expect(screen.IsDirty):ToBe(false)
        expect(screen.Gui.Parent):ToBe(UIService)
        expect(screen.Gui.DisplayOrder):ToBe(100)

        expect((find(screen.Gui, { "Backdrop", "Window", "Title" }) :: TextLabel).Text):ToBe("Options")
        expect((find(screen.Gui, { "Backdrop", "Window", "Bar", "Apply" }) :: TextButton).Text):ToBe("Apply")
        local caption = find(screen.Gui, { "Backdrop", "Window", "Body", "Graphics", "ShadowQuality", "Caption" })
        expect((caption :: TextLabel).Text):ToBe("Shadow quality")

        expect(screen:GetValueText("QualityLevel")):ToBe("High")
        expect(screen:GetValueText("RenderScale")):ToBe("100%")
        expect(screen:GetValueText("ShadowQuality")):ToBe("High")
        expect(screen:GetValueText("ShadowDistance")):ToBe("120 m")
        expect(screen:GetValueText("Bloom")):ToBe("On")
        expect(screen:GetValueText("WindowMode")):ToBe("Windowed")
        expect(screen:GetValueText("Resolution")):ToBe("Default")
        expect(screen:GetValueText("MaxFrameRate")):ToBe("Unlimited")

        -- Nothing draws by it yet, so a menu does not offer it.
        expect(Graphics:IsApplied("MotionBlur")):ToBe(false)
        expect(screen:GetValueText("MotionBlur")):ToBe(nil)
        expect(function()
            screen:Step("MotionBlur", 1)
        end):ToThrow()
        screen:Close()
    end)

    it("leaves out what the game hides, and takes the game's heading and place", function()
        fresh()
        local screen = settings.open({ Hide = { "Bloom", "WindowMode" }, Title = "Video", DisplayOrder = 7 })
        expect(screen:GetValueText("Bloom")):ToBe(nil)
        expect(screen:GetValueText("WindowMode")):ToBe(nil)
        expect(screen:GetValueText("SunRays")):ToBe("On")
        expect((find(screen.Gui, { "Backdrop", "Window", "Title" }) :: TextLabel).Text):ToBe("Video")
        expect(screen.Gui.DisplayOrder):ToBe(7)
        screen:Close()
    end)

    it("writes a quality row at once, one stop at a time", function()
        fresh()
        local screen = settings.open()
        expect(screen:Step("Bloom", 1)):ToBe(true)
        expect(Graphics.Bloom):ToBe(false)
        expect(screen:GetValueText("Bloom")):ToBe("Off")
        expect(screen.IsDirty):ToBe(true)

        expect(screen:Step("RenderScale", -1)):ToBe(true)
        expect(screen:GetValueText("RenderScale")):ToBe("90%")
        expect(math.abs(Graphics.RenderScale - 0.9) < 1e-4):ToBe(true)
        -- Past the last stop there is nothing, and nothing is written.
        expect(screen:Step("RenderScale", 1)):ToBe(true)
        expect(screen:Step("RenderScale", 1)):ToBe(false)
        expect(Graphics.RenderScale):ToBe(1)

        expect(screen:Step("ShadowQuality", -1)):ToBe(true)
        expect(Graphics.ShadowQuality):ToBe(Enum.ShadowQuality.Medium)
        expect(screen:Step("MaxFrameRate", 1)):ToBe(true)
        expect(Graphics.MaxFrameRate):ToBe(30)
        expect(screen:GetValueText("MaxFrameRate")):ToBe("30 FPS")
        screen:Close()
    end)

    it("steps from a value that is no stop to the stop beside it", function()
        fresh()
        Graphics.RenderScale = 0.85
        local screen = settings.open()
        expect(screen:GetValueText("RenderScale")):ToBe("85%")
        screen:Step("RenderScale", 1)
        expect(screen:GetValueText("RenderScale")):ToBe("90%")
        Graphics.RenderScale = 0.85
        screen:Step("RenderScale", -1)
        expect(screen:GetValueText("RenderScale")):ToBe("80%")
        screen:Close()
    end)

    it("steps the level, and from a mix of levels goes to the machine's own", function()
        fresh()
        local screen = settings.open()
        expect(screen:Step("QualityLevel", -1)):ToBe(true)
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Medium)
        expect(screen:GetValueText("QualityLevel")):ToBe("Medium")
        expect(screen:GetValueText("ShadowQuality")):ToBe("Medium")

        screen:Step("Bloom", 1)
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Custom)
        expect(screen:GetValueText("QualityLevel")):ToBe("Custom")
        expect(screen:Step("QualityLevel", 1)):ToBe(true)
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Auto)
        expect(screen:GetValueText("QualityLevel")):ToBe("Automatic")
        expect(Graphics.Bloom):ToBe(true)
        -- The machine's own is the first stop, and Ultra the last.
        expect(screen:Step("QualityLevel", -1)):ToBe(false)
        for _ = 1, 4 do
            expect(screen:Step("QualityLevel", 1)):ToBe(true)
        end
        expect(Graphics.QualityLevel):ToBe(Enum.GraphicsQuality.Ultra)
        expect(screen:Step("QualityLevel", 1)):ToBe(false)
        screen:Close()
    end)

    it("holds what moves the window until Apply, and keeps everything then", function()
        fresh()
        local screen = settings.open()
        expect(screen:Step("WindowMode", 1)):ToBe(true)
        expect(screen:GetValueText("WindowMode")):ToBe("Borderless")
        expect(Graphics.WindowMode):ToBe(Enum.WindowMode.Windowed)
        expect(screen.IsDirty):ToBe(true)
        screen:Step("Bloom", 1)

        expect(screen:Apply()):ToBe(true)
        expect(screen.IsDirty):ToBe(false)
        expect(Graphics.WindowMode):ToBe(Enum.WindowMode.Borderless)
        expect(Graphics:GetSource("WindowMode")):ToBe(Enum.SettingSource.Player)
        expect(Graphics:GetSource("Bloom")):ToBe(Enum.SettingSource.Player)
        expect(Graphics.Bloom):ToBe(false)
        screen:Close()
    end)

    it("goes back to what was kept on Revert, and to the project's on Defaults", function()
        fresh()
        local screen = settings.open()
        screen:Step("Bloom", 1)
        screen:Apply()
        screen:Step("SunRays", 1)
        screen:Step("WindowMode", 1)
        expect(Graphics.SunRays):ToBe(false)

        screen:Revert()
        expect(screen.IsDirty):ToBe(false)
        expect(Graphics.SunRays):ToBe(true)
        expect(Graphics.Bloom):ToBe(false)
        expect(screen:GetValueText("WindowMode")):ToBe("Windowed")
        expect(screen:GetValueText("Bloom")):ToBe("Off")

        screen:ResetToDefaults()
        expect(Graphics.Bloom):ToBe(true)
        expect(screen:GetValueText("Bloom")):ToBe("On")
        expect(screen.IsDirty):ToBe(true)
        screen:Close()
    end)

    it("puts back what was not applied when it is closed, and says so once", function()
        fresh()
        local screen = settings.open()
        local closed = 0
        screen.Closed:Connect(function()
            closed += 1
        end)
        screen:Step("Bloom", 1)
        expect(Graphics.Bloom):ToBe(false)

        screen:Close()
        screen:Close()
        expect(screen.IsOpen):ToBe(false)
        expect(screen.Gui.Parent):ToBe(nil)
        expect(screen:Step("SunRays", 1)):ToBe(false)
        task.wait()
        task.wait()
        expect(closed):ToBe(1)
        expect(Graphics.Bloom):ToBe(true)
        expect(Graphics.SunRays):ToBe(true)
    end)

    it("gives the selection back to what had it", function()
        fresh()
        local under = Instance.new("ScreenGui")
        local play = Instance.new("TextButton")
        play.Size = UDim2.fromOffset(100, 40)
        play.Parent = under
        under.Parent = UIService
        UIService.SelectedObject = play

        local screen = settings.open()
        local selected = UIService.SelectedObject
        expect(selected ~= nil and selected:IsDescendantOf(screen.Gui)):ToBe(true)
        screen:Close()
        expect(UIService.SelectedObject):ToBe(play)

        -- A player on a mouse had nothing selected, and gets no outline.
        UIService.SelectedObject = nil
        local second = settings.open()
        expect(UIService.SelectedObject):ToBe(nil)
        second:Close()
        under:Destroy()
    end)

    it("is written in the player's language, and a game's own word wins", function()
        fresh()
        local screen = settings.open()
        local apply = find(screen.Gui, { "Backdrop", "Window", "Bar", "Apply" }) :: TextButton
        expect(apply.Text):ToBe("Apply")
        Localization.Locale = "pt-BR"
        task.wait()
        task.wait()
        -- The project's pt-BR catalog says this key; the rest is the engine's.
        expect(apply.Text):ToBe("Aplicar")
        expect((find(screen.Gui, { "Backdrop", "Window", "Bar", "Back" }) :: TextButton).Text):ToBe("Back")
        screen:Close()
        Localization.Locale = "en"
    end)
end)
