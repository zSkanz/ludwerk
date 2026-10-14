--!strict
--- `@engine/subtitles` (ADR 0200): what is being said, on the screen, in one
--- call -- who says it, in the language the player reads, shown as the player
--- says and sized as the player says.
---
--- Over the lines in `content/dialogue/spec.lines.json`. `spec.written` has no
--- recording, so under `Auto` it is always wanted; `spec.hello` is recorded in
--- English and "in Portuguese".

local subtitles = require("@engine/subtitles")
local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local DialogueService = game:GetService("DialogueService")
local Localization = game:GetService("LocalizationService")
local PhysicsService = game:GetService("PhysicsService")
local UIService = game:GetService("UIService")

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
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

describe("@engine/subtitles", function()
    local display: subtitles.Display? = nil

    afterEach(function()
        if display then
            display:Unmount()
            display = nil
        end
        Localization.Locale = "en"
        Localization.VoiceLocale = ""
        DialogueService.Subtitles = Enum.SubtitleMode.Auto
        DialogueService.SubtitleScale = 1
        DialogueService.SubtitleBackground = 0.6
    end)

    it("puts a screen over the game, and takes it away", function()
        local mounted = subtitles.mount()
        display = mounted
        expect(mounted.IsMounted):ToBe(true)
        expect(mounted.Gui.Parent):ToBe(UIService)
        expect(mounted.Gui.DisplayOrder):ToBe(90)
        expect(mounted.Gui.KeepOnSceneLoad):ToBe(true)
        expect(#mounted:GetLines()):ToBe(0)
        mounted:Unmount()
        expect(mounted.IsMounted):ToBe(false)
        expect(mounted.Gui.Parent):ToBeNil()
    end)

    it("shows who is speaking and what they say, for as long as they say it", function()
        local mounted = subtitles.mount()
        display = mounted
        local sound = DialogueService:Say("spec.written")
        step(4)
        local lines = mounted:GetLines()
        expect(#lines):ToBe(1)
        expect(lines[1]):ToBe("Guide: Not recorded yet.")
        -- The speaker's name is in the speaker's colour.
        local words = find(mounted.Gui, { "Lines", "Line", "Words" }) :: TextLabel
        expect(words.RichText):ToBe(true)
        expect(string.find(words.Text, '<font color="#e8b04a">Guide</font>', 1, true) ~= nil):ToBe(true)

        -- A quarter of a second, and it is gone with the line.
        step(24)
        expect(#mounted:GetLines()):ToBe(0)
        expect(sound.Parent):ToBeNil()
    end)

    it("is read in the language the player reads", function()
        Localization.Locale = "pt-BR"
        local mounted = subtitles.mount()
        display = mounted
        local sound = DialogueService:Say("spec.written")
        step(3)
        -- The speaker's name has a Portuguese word; the line's text has none
        -- and is read in the project's default language.
        expect(mounted:GetLines()[1]):ToBe("Guia: Not recorded yet.")
        -- And follows a change of language while it is on the screen.
        Localization.Locale = "en"
        step(2)
        expect(mounted:GetLines()[1]):ToBe("Guide: Not recorded yet.")
        sound:Destroy()
    end)

    it("is shown as the player says: when needed, always, never", function()
        local mounted = subtitles.mount()
        display = mounted
        -- Heard in English and read in English: nothing is needed.
        local heard = DialogueService:Say("spec.hello")
        step(3)
        expect(#mounted:GetLines()):ToBe(0)
        DialogueService.Subtitles = Enum.SubtitleMode.On
        step(2)
        expect(#mounted:GetLines()):ToBe(1)
        expect(mounted:GetLines()[1]):ToBe("Guide: Hello.")
        DialogueService.Subtitles = Enum.SubtitleMode.Off
        step(2)
        expect(#mounted:GetLines()):ToBe(0)
        heard:Destroy()

        -- A voice in one language and the text in another is the case
        -- subtitles are for: back on "when needed", it is shown.
        DialogueService.Subtitles = Enum.SubtitleMode.Auto
        Localization.VoiceLocale = "pt-BR"
        local dubbed = DialogueService:Say("spec.hello")
        step(3)
        expect(#mounted:GetLines()):ToBe(1)
        dubbed:Destroy()
    end)

    it("is sized and backed as the player says", function()
        DialogueService.Subtitles = Enum.SubtitleMode.On
        local mounted = subtitles.mount({ TextSize = 20 })
        display = mounted
        local sound = DialogueService:Say("spec.written")
        step(3)
        local line = find(mounted.Gui, { "Lines", "Line" }) :: Frame
        local words = line:FindFirstChild("Words") :: TextLabel
        expect(words.TextSize):ToBe(20)
        expect(line.BackgroundTransparency):ToBeCloseTo(0.4, 4)

        DialogueService.SubtitleScale = 1.5
        DialogueService.SubtitleBackground = 1
        step(2)
        expect(words.TextSize):ToBe(30)
        expect(line.BackgroundTransparency):ToBeCloseTo(0, 4)
        sound:Destroy()
    end)

    it("keeps the newest few when more are speaking than fit", function()
        DialogueService.Subtitles = Enum.SubtitleMode.On
        local mounted = subtitles.mount({ MaxCaptions = 2 })
        display = mounted
        local first = DialogueService:Say("spec.aside")
        step(2)
        local second = DialogueService:Say("spec.written")
        local third = DialogueService:Say("spec.written")
        step(3)
        local lines = mounted:GetLines()
        expect(#lines):ToBe(2)
        -- The aside, said first, is the one that gave way; it has no speaker.
        expect(lines[1]):ToBe("Guide: Not recorded yet.")
        first:Destroy()
        second:Destroy()
        third:Destroy()
    end)
end)
