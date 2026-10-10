--!strict
--- Dubbing (ADR 0200) as a script meets it: a voice language apart from the
--- text language, the player's volumes, a line said in one call with its
--- caption, and how loud what is playing is.
---
--- Over the suite's own content: `content/voice/spec/hello.wav` is a fifth of
--- a second of a 1,000 Hz tone, and `content/l10n/pt-BR/voice/spec/hello.wav`
--- is the same line "in Portuguese" -- two fifths of a second, at 200 Hz, so
--- the two can be told apart by ear. The lines are in
--- `content/dialogue/spec.lines.json`.
---
--- What the mixer does with a volume, and what a line costs the tick, are
--- asserted in C++ (`engine_audio_tests`, the determinism scenario `dubbed`,
--- the two-process gate `netcode_dubbed`).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local AudioService = game:GetService("AudioService")
local DialogueService = game:GetService("DialogueService")
local Localization = game:GetService("LocalizationService")
local PhysicsService = game:GetService("PhysicsService")

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
end

-- Every case leaves the player as it found them.
local function reset()
    Localization.Locale = "en"
    Localization.VoiceLocale = ""
    AudioService.PlayerVolume = 1
    AudioService.MusicVolume = 1
    AudioService.EffectsVolume = 1
    AudioService.VoiceVolume = 1
    DialogueService.Subtitles = Enum.SubtitleMode.Auto
    DialogueService.SubtitleScale = 1
    DialogueService.SubtitleBackground = 0.6
end

describe("a voice language apart from the text language", function()
    afterEach(reset)

    it("lists the languages this machine can play, the default first", function()
        local voices = Localization:GetVoiceLocales()
        expect(#voices):ToBe(2)
        expect(voices[1]):ToBe("en")
        expect(voices[2]):ToBe("pt-BR")
    end)

    it("follows the text language until the player chooses one", function()
        Localization.Locale = "en"
        expect(Localization.VoiceFollowsLocale):ToBe(true)
        expect(Localization.VoiceLocale):ToBe("en")
        Localization.Locale = "pt-BR"
        expect(Localization.VoiceLocale):ToBe("pt-BR")

        -- Chosen, it stays where it was put whatever is read.
        Localization.VoiceLocale = "en"
        expect(Localization.VoiceFollowsLocale):ToBe(false)
        expect(Localization.VoiceLocale):ToBe("en")
        Localization.Locale = "en"
        Localization.Locale = "pt-BR"
        expect(Localization.VoiceLocale):ToBe("en")

        -- Written empty, it follows again.
        Localization.VoiceLocale = ""
        expect(Localization.VoiceFollowsLocale):ToBe(true)
        expect(Localization.VoiceLocale):ToBe("pt-BR")
    end)

    it("keeps a choice of the language that was already being heard (D622)", function()
        Localization.Locale = "en"
        expect(Localization.VoiceFollowsLocale):ToBe(true)
        local said = 0
        local connection = Localization.VoiceLocaleChanged:Connect(function()
            said += 1
        end)
        -- English is heard because English is read. Choosing English changes
        -- nothing that is heard now -- and is a choice all the same.
        Localization.VoiceLocale = "en"
        expect(Localization.VoiceFollowsLocale):ToBe(false)
        Localization.Locale = "pt-BR"
        expect(Localization.VoiceLocale):ToBe("en")
        task.wait()
        -- Nothing that is heard moved, so nothing was said of it.
        expect(said):ToBe(0)
        connection:Disconnect()
        Localization.Locale = "en"
        Localization.VoiceLocale = ""
    end)

    it("narrows a language to a voice there is, and refuses one there is none for", function()
        Localization.VoiceLocale = "pt"
        expect(Localization.VoiceLocale):ToBe("pt-BR")
        expect(function()
            Localization.VoiceLocale = "ja"
        end):ToThrow()
        expect(Localization.VoiceLocale):ToBe("pt-BR")
    end)

    it("says VoiceLocaleChanged for a change, whichever of the two languages moved it", function()
        Localization.Locale = "en"
        task.wait()
        local heard: { string } = {}
        local connection = Localization.VoiceLocaleChanged:Connect(function(locale)
            table.insert(heard, locale)
        end)
        -- The text moved, and the voice followed it.
        Localization.Locale = "pt-BR"
        task.wait()
        task.wait()
        expect(#heard):ToBe(1)
        expect(heard[1]):ToBe("pt-BR")
        -- Chosen back, and chosen the same again: one change.
        Localization.VoiceLocale = "en"
        Localization.VoiceLocale = "en"
        task.wait()
        task.wait()
        expect(#heard):ToBe(2)
        expect(heard[2]):ToBe("en")
        connection:Disconnect()
    end)

    it("gives the system's own languages as a list of names", function()
        local system = Localization:GetSystemLocales()
        expect(system):ToBeA("table")
        for _, name in system do
            expect(name):ToBeA("string")
        end
    end)
end)

describe("the player's volumes", function()
    afterEach(reset)

    it("are full until the player moves them, and are kept as written", function()
        expect(AudioService.PlayerVolume):ToBe(1)
        expect(AudioService.MusicVolume):ToBe(1)
        expect(AudioService.EffectsVolume):ToBe(1)
        expect(AudioService.VoiceVolume):ToBe(1)
        AudioService.VoiceVolume = 0.25
        AudioService.MusicVolume = 0.5
        expect(AudioService.VoiceVolume):ToBe(0.25)
        expect(AudioService.MusicVolume):ToBe(0.5)
        -- The game's own master is another number.
        expect(AudioService.MasterVolume):ToBe(1)
    end)

    it("are from nothing to full", function()
        expect(function()
            AudioService.VoiceVolume = 1.5
        end):ToThrow()
        expect(function()
            AudioService.MusicVolume = -0.1
        end):ToThrow()
        expect(AudioService.VoiceVolume):ToBe(1)
    end)

    it("leave how far music goes under a voice to the game", function()
        expect(AudioService.MusicUnderVoice):ToBeCloseTo(0.4, 5)
        AudioService.MusicUnderVoice = 0.7
        expect(AudioService.MusicUnderVoice):ToBeCloseTo(0.7, 5)
        AudioService.MusicUnderVoice = 0.4
    end)

    it("a sound is an effect until it is said to be music or a voice", function()
        local sound = Instance.new("Sound")
        expect(sound.Category):ToBe(Enum.SoundCategory.Effects)
        sound.Category = Enum.SoundCategory.Music
        expect(sound.Category):ToBe(Enum.SoundCategory.Music)
        sound:Destroy()
    end)
end)

describe("a line said in one call", function()
    afterEach(reset)

    it("is a voice with its caption, as long as its longest language", function()
        local sound = DialogueService:Say("spec.hello")
        expect(sound.Category):ToBe(Enum.SoundCategory.Voice)
        expect(sound.Playing):ToBe(true)
        expect(sound.Content):ToBe("asset://voice/spec/hello.wav")
        local caption = sound:FindFirstChildOfClass("Caption") :: Caption
        expect(caption):ToBeTruthy()
        -- Its text is the line's own name, and its speaker the name the
        -- lines file gave.
        expect(caption.Text):ToBe("spec.hello")
        expect(caption.Speaker):ToBe("spec.speaker.guide")
        expect(Localization:Translate(caption.Speaker)):ToBe("Guide")
        step(2)
        -- A fifth of a second is being heard, and the line is two fifths:
        -- the Portuguese recording's, whoever listens.
        expect(sound.TimeLength):ToBeCloseTo(0.4, 3)
        sound:Destroy()
    end)

    it("is current while it is said, and gone when it has been", function()
        local started: { Caption } = {}
        local ended: { Caption } = {}
        local began = DialogueService.CaptionStarted:Connect(function(caption: Caption)
            table.insert(started, caption)
        end)
        local over = DialogueService.CaptionEnded:Connect(function(caption: Caption)
            table.insert(ended, caption)
        end)

        local sound = DialogueService:Say("spec.written")
        local caption = sound:FindFirstChildOfClass("Caption") :: Caption
        -- Counted by the caption: a line a case before this one cut short
        -- may be ending as this one begins.
        local function times(list: { Caption }): number
            local count = 0
            for _, other in list do
                if other == caption then
                    count += 1
                end
            end
            return count
        end
        step(4)
        expect(times(started)):ToBe(1)
        local current = DialogueService:GetCaptions()
        expect(#current):ToBe(1)
        expect(current[1]):ToBe(caption)

        -- A quarter of a second, as its entry says; then a moment.
        step(24)
        expect(#DialogueService:GetCaptions()):ToBe(0)
        expect(times(started)):ToBe(1)
        expect(times(ended)):ToBe(1)
        -- Said, and not left behind.
        expect(sound.Parent):ToBeNil()
        began:Disconnect()
        over:Disconnect()
    end)

    it("refuses a line no file has", function()
        expect(function()
            DialogueService:Say("spec.nobody_wrote_this")
        end):ToThrow()
    end)

    it("shows a caption as the player says, and under Auto when what is heard is not what is read", function()
        Localization.Locale = "en"
        local recorded = DialogueService:Say("spec.hello")
        local written = DialogueService:Say("spec.written")
        local heard = recorded:FindFirstChildOfClass("Caption") :: Caption
        local read = written:FindFirstChildOfClass("Caption") :: Caption
        step(2)

        -- Heard in English and read in English: no caption is wanted.
        expect(DialogueService:ShowsCaption(heard)):ToBe(false)
        -- Nobody recorded this one: its caption is all there is of it.
        expect(DialogueService:ShowsCaption(read)):ToBe(true)

        DialogueService.Subtitles = Enum.SubtitleMode.On
        expect(DialogueService:ShowsCaption(heard)):ToBe(true)
        DialogueService.Subtitles = Enum.SubtitleMode.Off
        expect(DialogueService:ShowsCaption(heard)):ToBe(false)
        expect(DialogueService:ShowsCaption(read)):ToBe(false)
        recorded:Destroy()
        written:Destroy()

        -- A voice in one language and the text in another is the case a
        -- caption exists for.
        DialogueService.Subtitles = Enum.SubtitleMode.Auto
        Localization.VoiceLocale = "pt-BR"
        local dubbed = DialogueService:Say("spec.hello")
        local foreign = dubbed:FindFirstChildOfClass("Caption") :: Caption
        step(2)
        expect(DialogueService:ShowsCaption(foreign)):ToBe(true)
        dubbed:Destroy()
    end)

    it("keeps how the player wants subtitles", function()
        expect(DialogueService.Subtitles):ToBe(Enum.SubtitleMode.Auto)
        expect(DialogueService.SubtitleScale):ToBe(1)
        expect(DialogueService.SubtitleBackground):ToBeCloseTo(0.6, 5)
        DialogueService.SubtitleScale = 1.5
        DialogueService.SubtitleBackground = 0
        expect(DialogueService.SubtitleScale):ToBe(1.5)
        expect(DialogueService.SubtitleBackground):ToBe(0)
        expect(function()
            DialogueService.SubtitleScale = 5
        end):ToThrow()
        expect(function()
            DialogueService.SubtitleBackground = 2
        end):ToThrow()
    end)
end)

describe("how loud what is playing is", function()
    afterEach(reset)

    it("is nothing for a sound that is not playing", function()
        local sound = Instance.new("Sound")
        sound.Content = "asset://voice/spec/hello.wav"
        sound.Parent = workspace
        expect(sound.Loudness):ToBe(0)
        local low, mid, high = sound:GetBands()
        expect(low + mid + high):ToBe(0)
        sound:Destroy()
    end)

    it("is the recording's own level, and its bands tell one pitch from another", function()
        -- English: a tone at 1,000 Hz, nearly full scale.
        local sound = DialogueService:Say("spec.hello")
        -- Turned right down: a mouth opens the same whatever a slider says.
        sound.Volume = 0.01
        step(6)
        expect(sound.Loudness > 0.9):ToBe(true)
        local low, mid, high = sound:GetBands()
        expect(mid > 0.8):ToBe(true)
        expect(low < 0.2):ToBe(true)
        expect(high < 0.2):ToBe(true)
        -- And everything of its kind that can be heard.
        local _, voices = AudioService:GetBands(Enum.SoundCategory.Voice)
        expect(voices > 0.0):ToBe(true)
        local _, music = AudioService:GetBands(Enum.SoundCategory.Music)
        expect(music):ToBe(0)
        sound:Destroy()

        -- Portuguese: the same line is a tone at 200 Hz.
        Localization.VoiceLocale = "pt-BR"
        local dubbed = DialogueService:Say("spec.hello")
        step(6)
        local dubbedLow, dubbedMid = dubbed:GetBands()
        expect(dubbedLow > 0.8):ToBe(true)
        expect(dubbedMid < 0.2):ToBe(true)
        dubbed:Destroy()
    end)
end)
