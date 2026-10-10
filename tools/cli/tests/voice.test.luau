--!strict
--[[
	`ludwerk voice` and the dialogue half of `ludwerk check` (ADR 0200 §8):
	what a lines file says, and what each language still lacks of it.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local process = require("@std/process")
local test = require("@std/test")

local check = require("../commands/check")
local dialogue = require("../dialogue")
local projectlib = require("../project")
local voice = require("../commands/voice")

local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    local root = `{base}/scratch-voice-{name}`
    if fs.exists(root) then
        fs.removeDirectory(root, { recursive = true })
    end
    fs.createDirectory(root, { makeParents = true })
    return root
end

local function write(root: string, file: string, text: string)
    local folder = string.match(file, "^(.*)/[^/]+$")
    if folder ~= nil then
        fs.createDirectory(`{root}/{folder}`, { makeParents = true })
    end
    fs.writeStringToFile(`{root}/{file}`, text)
end

--- A mono 16-bit `.wav` of `frames` frames at `rate`: the smallest file that
--- says how long it is. Its samples avoid the byte a text-mode write would
--- change.
local function wav(rate: number, frames: number): string
    local data = string.rep("\1\2", frames)
    return "RIFF"
        .. string.pack("<I4", 36 + #data)
        .. "WAVEfmt "
        .. string.pack("<I4I2I2I4I4I2I2", 16, 1, 1, rate, rate * 2, 2, 16)
        .. "data"
        .. string.pack("<I4", #data)
        .. data
end

local function languageOf(report: dialogue.Report, locale: string): dialogue.Language
    for _, language in report.languages do
        if language.locale == locale then
            return language
        end
    end
    error(`the report has no language {locale}`)
end

--- Two speakers and five lines, in English; Portuguese with its text and
--- most of its voices; Japanese with voices and no text; Spanish with text
--- and no voice.
local function project(name: string): string
    local root = scratch(name)
    write(root, "project.toml", '[project]\nname = "Voices"\ndefault_locale = "en"\n')
    write(
        root,
        "content/dialogue/act1.lines.json",
        [[{
  "speakers": {
    "mara": { "name": "speaker.mara", "color": "#E8B04A" },
    "tom": { "name": "speaker.tom" }
  },
  "lines": {
    "intro_01": { "speaker": "mara" },
    "intro_02": { "speaker": "tom", "text": "shared.greeting", "sound": "asset://voice/shared/hello.wav", "seconds": 2.5 },
    "intro_03": {},
    "sign": { "sound": "" },
    "late": { "speaker": "mara", "sound": "asset://voice/act1/late.wav" }
  }
}]]
    )
    write(
        root,
        "i18n/en.json",
        '{"act1.intro_01": "Hello.", "shared.greeting": "Hi!", "act1.intro_03": "Well.", "act1.sign": "[a sign]",'
            .. ' "act1.late": "Late.", "speaker.mara": "Mara", "speaker.tom": "Tom"}'
    )
    write(
        root,
        "i18n/pt-BR.json",
        '{"act1.intro_01": "Ola.", "shared.greeting": "Oi!", "act1.sign": "[uma placa]", "speaker.mara": "Mara"}'
    )
    write(root, "i18n/es.json", '{"act1.intro_01": "Hola."}')

    -- The default language: two of the four voiced lines, and a take nobody
    -- uses any more.
    write(root, "content/voice/act1/intro_01.ogg", "a recording")
    write(root, "content/voice/shared/hello.wav", "a recording")
    write(root, "content/voice/act1/old_take.ogg", "a recording")
    -- Portuguese: one of them, the line English has not recorded yet, and a
    -- file for a line that is gone.
    write(root, "content/l10n/pt-BR/voice/act1/intro_01.ogg", "a recording")
    write(root, "content/l10n/pt-BR/voice/act1/late.wav", "a recording")
    write(root, "content/l10n/pt-BR/voice/act1/cut.ogg", "a recording")
    write(root, "content/l10n/pt-BR/ui/logo.png", "a picture")
    -- Japanese: the greeting.
    write(root, "content/l10n/ja/voice/shared/hello.wav", "a recording")
    return root
end

test.suite("voice", function(suite)
    suite:case("a lines file is read as the engine reads it", function()
        local root = project("read")
        local document = dialogue.read(`{root}/content`)
        assert.eq(document.files, 1)
        assert.eq(#document.problems, 0)
        assert.eq(#document.lines, 5)
        -- By id: a line is `<name>.<id>`, its text that id and its sound
        -- `voice/<name>/<id>.ogg`, unless its entry says otherwise.
        local byId: { [string]: dialogue.Line } = {}
        for _, line in document.lines do
            byId[line.id] = line
        end
        assert.eq(document.lines[1].id, "act1.intro_01")
        assert.eq(byId["act1.intro_01"].text, "act1.intro_01")
        assert.eq(byId["act1.intro_01"].sound, "voice/act1/intro_01.ogg")
        assert.eq(byId["act1.intro_02"].text, "shared.greeting")
        assert.eq(byId["act1.intro_02"].sound, "voice/shared/hello.wav")
        assert.eq(byId["act1.intro_03"].sound, "voice/act1/intro_03.ogg")
        -- An empty sound is a line that is never voiced.
        assert.eq(byId["act1.sign"].sound, nil)
        assert.eq(table.concat(document.speakerNames, ","), "speaker.mara,speaker.tom")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("each language's missing text, recordings, speakers and unused files", function()
        local root = project("report")
        local found = check.voiceReport(root, false)
        assert.that(found ~= nil, "a project with a lines file has a report")
        local report = found :: dialogue.Report
        assert.eq(report.defaultLocale, "en")
        assert.eq(report.files, 1)
        assert.eq(report.lines, 5)
        assert.eq(#report.problems, 0)
        assert.eq(report.measured, false)
        -- The default first, then every catalog and every folder by name.
        local names: { string } = {}
        for _, language in report.languages do
            table.insert(names, language.locale)
        end
        assert.eq(table.concat(names, ","), "en,es,ja,pt-BR")

        -- (c) Recorded in no language, once: played as text.
        assert.eq(table.concat(report.silent, ","), "act1.intro_03")

        local english = languageOf(report, "en")
        assert.eq(english.default, true)
        assert.eq(english.catalog, true)
        assert.eq(#english.noText, 0)
        -- (b) Another language has it and this one does not.
        assert.eq(table.concat(english.noRecording, ","), "act1.late")
        -- (d) A file under its `voice/` that no line names.
        assert.eq(table.concat(english.unusedSounds, ","), "voice/act1/old_take.ogg")
        assert.eq(#english.noSpeakerName, 0)

        local portuguese = languageOf(report, "pt-BR")
        assert.eq(portuguese.catalog, true)
        assert.eq(portuguese.voiced, true)
        -- (a) Read in English.
        assert.eq(table.concat(portuguese.noText, ","), "act1.intro_03,act1.late")
        -- (b) Heard in English. The line recorded nowhere is not said again.
        assert.eq(table.concat(portuguese.noRecording, ","), "act1.intro_02")
        assert.eq(table.concat(portuguese.unusedSounds, ","), "l10n/pt-BR/voice/act1/cut.ogg")
        -- (e)
        assert.eq(table.concat(portuguese.noSpeakerName, ","), "speaker.tom")

        -- Voices and no catalog: every line is read in the default.
        local japanese = languageOf(report, "ja")
        assert.eq(japanese.catalog, false)
        assert.eq(japanese.voiced, true)
        assert.eq(#japanese.noText, 5)
        assert.eq(table.concat(japanese.noRecording, ","), "act1.intro_01,act1.late")
        assert.eq(#japanese.noSpeakerName, 0)

        -- Text and no voice: every recorded line is heard in the default.
        local spanish = languageOf(report, "es")
        assert.eq(spanish.catalog, true)
        assert.eq(spanish.voiced, false)
        assert.eq(#spanish.noText, 4)
        assert.eq(#spanish.noRecording, 3)
        assert.eq(#spanish.unusedSounds, 0)

        local sums = dialogue.totals(report)
        assert.eq(sums.untranslatedDefault, 0)
        -- Each line once, however many languages lack it.
        assert.eq(sums.untranslated, 5)
        assert.eq(sums.unrecorded, 3)
        assert.eq(sums.unused, 2)

        -- None of that is a mistake: the command answers 0, for the project
        -- and for one language of it, and 2 for a language it does not have.
        assert.eq(voice.run({ path = root, locale = nil, all = false }), 0)
        assert.eq(voice.run({ path = root, locale = "pt-BR", all = true }), 0)
        assert.eq(voice.run({ path = root, locale = "de", all = false }), 2)
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a language's own language answers for it: pt for pt-BR", function()
        -- ADR 0200 §1: a name is looked for under `l10n/pt-BR/`, then
        -- `l10n/pt/`, then as itself.
        assert.eq(dialogue.parentLanguage("pt-BR"), "pt")
        assert.eq(dialogue.parentLanguage("zh_Hans"), "zh")
        assert.eq(dialogue.parentLanguage("ja"), nil)

        local root = scratch("parent")
        write(root, "content/dialogue/a.lines.json", '{"lines": {"one": {}}}')
        write(root, "i18n/en.json", '{"a.one": "One."}')
        write(root, "i18n/pt-BR.json", '{"a.one": "Um."}')
        write(root, "content/voice/a/one.ogg", "a recording")
        write(root, "content/l10n/pt/voice/a/one.ogg", "a recording")
        local report = check.voiceReport(root, false) :: dialogue.Report
        local brazilian = languageOf(report, "pt-BR")
        assert.eq(brazilian.voiced, true)
        assert.eq(#brazilian.noRecording, 0)
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("what the engine would refuse is an error, and the command answers 1", function()
        local root = scratch("errors")
        write(root, "project.toml", '[project]\nname = "Errors"\n')
        write(root, "i18n/en.json", '{"good.one": "One."}')
        write(root, "content/dialogue/good.lines.json", '{"lines": {"one": {"sound": ""}}}')
        write(root, "content/dialogue/broken.lines.json", '{"lines": {"one": {}')
        write(root, "content/dialogue/shape.lines.json", '{"lines": ["one"]}')
        write(
            root,
            "content/dialogue/fields.lines.json",
            '{"speakers": {"mara": {"name": "speaker.mara"}, "nobody": {"color": "#fff"}},'
                .. ' "lines": {"a": {"speaker": "ghost"}, "b": {"seconds": "long"}, "c": {"sound": "voice/c.ogg"},'
                .. ' "d": 4, "e": {"speaker": "nobody"}}}'
        )
        local report = check.voiceReport(root, false) :: dialogue.Report
        assert.eq(report.files, 4)
        local said: { string } = {}
        for _, problem in report.problems do
            table.insert(said, `{problem.file}|{problem.message}`)
        end
        local all = table.concat(said, "\n")
        -- A file that does not parse, and one whose lines are not a table.
        assert.strContains(all, "dialogue/broken.lines.json|")
        assert.strContains(all, "dialogue/shape.lines.json|")
        -- A speaker the file does not have; a speaker with no name; a field
        -- of the wrong kind; a sound that is not a content name; a line that
        -- is not an entry.
        assert.strContains(all, '"ghost"')
        assert.strContains(all, '"nobody"')
        assert.strContains(all, '"b"')
        assert.strContains(all, '"voice/c.ogg"')
        assert.strContains(all, '"d"')
        assert.eq(#report.problems, 7)
        -- A speaker with no name is one error, not one more for each line of
        -- theirs.
        assert.strNotContains(all, 'the line "e"')
        -- The file that is right is still read.
        assert.eq(report.lines >= 1, true)
        assert.eq(voice.run({ path = root, locale = nil, all = false }), 1)
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a recording with no viseme track beside it is listed, a language its own", function()
        local root = project("tracks")
        -- English has a track for one of its two recordings; Portuguese has
        -- one for the line English has not recorded.
        write(root, "content/voice/act1/intro_01.visemes.json", '{ "cues": [] }')
        write(root, "content/l10n/pt-BR/voice/act1/late.visemes.json", '{ "cues": [] }')
        local report = check.voiceReport(root, false) :: dialogue.Report

        local english = languageOf(report, "en")
        assert.eq(#english.noTrack, 1)
        assert.eq(english.noTrack[1], "voice/shared/hello.wav")
        -- Its own recordings only: a line it hears in English is English's.
        local portuguese = languageOf(report, "pt-BR")
        assert.eq(#portuguese.noTrack, 1)
        assert.eq(portuguese.noTrack[1], "l10n/pt-BR/voice/act1/intro_01.ogg")
        local japanese = languageOf(report, "ja")
        assert.eq(#japanese.noTrack, 1)
        assert.eq(japanese.noTrack[1], "l10n/ja/voice/shared/hello.wav")
        -- A language with nothing recorded has no track to be missing.
        assert.eq(#languageOf(report, "es").noTrack, 0)

        -- With no tool on the machine, asking for them makes none and says
        -- so; the report is still given.
        local before = process.env.ENG_LIPSYNC_TOOL
        process.env.ENG_LIPSYNC_TOOL = `{root}/there-is-no-such-tool`
        assert.eq(voice.run({ path = root, locale = nil, all = false, visemes = true }), 0)
        process.env.ENG_LIPSYNC_TOOL = before
        assert.that(not fs.exists(`{root}/content/voice/shared/hello.visemes.json`), "a track was made by no tool")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a project with no lines file is none of this command's", function()
        local root = scratch("none")
        write(root, "project.toml", '[project]\nname = "None"\n')
        write(root, "i18n/en.json", '{"menu.play": "Play"}')
        write(root, "content/voice/jingle.ogg", "a recording")
        write(root, "content/l10n/pt-BR/voice/jingle.ogg", "a recording")
        write(root, "content/dialogue/notes.json", "{}")
        assert.eq(check.voiceReport(root, false), nil)
        assert.eq(voice.run({ path = root, locale = nil, all = false }), 0)
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a line whose languages are of different lengths is pointed at, furthest apart first", function()
        -- ADR 0200 §2: a localized sound lasts as long as its longest
        -- language, so one long recording is silence in every other.
        local root = scratch("spread")
        write(root, "project.toml", '[project]\nname = "Spread"\ndefault_locale = "en"\n')
        write(
            root,
            "content/dialogue/act1.lines.json",
            '{"lines": {"close": {"sound": "asset://voice/act1/close.wav"},'
                .. ' "far": {"sound": "asset://voice/act1/far.wav"},'
                .. ' "alone": {"sound": "asset://voice/act1/alone.wav"}}}'
        )
        write(root, "i18n/en.json", '{"act1.close": "Close.", "act1.far": "Far.", "act1.alone": "Alone."}')
        -- One second in English, a second and a half in Portuguese -- recorded
        -- at another rate, so the lengths are compared in time and not in the
        -- files' own frames -- and a little over one in Japanese.
        write(root, "content/voice/act1/far.wav", wav(48000, 48000))
        write(root, "content/l10n/pt-BR/voice/act1/far.wav", wav(24000, 36000))
        write(root, "content/l10n/ja/voice/act1/far.wav", wav(48000, 52800))
        -- A fifth of a second apart: under the limit.
        write(root, "content/voice/act1/close.wav", wav(48000, 48000))
        write(root, "content/l10n/pt-BR/voice/act1/close.wav", wav(48000, 57600))
        -- One language only: nothing to compare.
        write(root, "content/voice/act1/alone.wav", wav(48000, 4800))

        -- The arithmetic, on lengths as the asset compiler gives them.
        local given: dialogue.Lengths = {
            rate = 48000,
            files = {
                ["voice/act1/far.wav"] = 48000,
                ["l10n/pt-BR/voice/act1/far.wav"] = 72000,
                ["l10n/ja/voice/act1/far.wav"] = 52800,
                ["voice/act1/close.wav"] = 48000,
                ["l10n/pt-BR/voice/act1/close.wav"] = 57600,
            },
            unmeasured = {},
        }
        local function verify(report: dialogue.Report)
            assert.eq(report.measured, true)
            assert.eq(#report.spreads, 2)
            assert.eq(report.flagged, 1)
            local far = report.spreads[1]
            assert.eq(far.line, "act1.far")
            assert.eq(far.flagged, true)
            assert.eq(far.shortestLocale, "en")
            assert.eq(far.longestLocale, "pt-BR")
            assert.eq(string.format("%.3f", far.shortest), "1.000")
            assert.eq(string.format("%.3f", far.longest), "1.500")
            assert.eq(string.format("%.3f", far.spread), "0.500")
            local close = report.spreads[2]
            assert.eq(close.line, "act1.close")
            assert.eq(close.flagged, false)
            assert.eq(string.format("%.3f", close.spread), "0.200")
            assert.eq(close.longestLocale, "pt-BR")
        end
        local computed = dialogue.report({
            root = root,
            content = `{root}/content`,
            defaultLocale = "en",
            catalogKeys = check.catalogKeys,
            lengths = given,
        })
        verify(computed :: dialogue.Report)
        assert.eq(dialogue.SpreadLimit, 0.35)

        -- And on the files themselves, measured by the compiler that is
        -- built here -- where one is.
        if projectlib.findTool("assetc") == nil then
            print("no asset compiler is built here, so the recordings were not measured from their files")
            local unmeasured = check.voiceReport(root, true) :: dialogue.Report
            assert.eq(unmeasured.measured, false)
            assert.eq(#unmeasured.spreads, 0)
        else
            local lengths = dialogue.measure(`{root}/content`)
            if lengths == nil then
                print("the asset compiler found here is older than `l10n-index --per-file`, so nothing was measured")
            else
                assert.eq(lengths.rate, 48000)
                assert.eq(lengths.files["l10n/pt-BR/voice/act1/far.wav"], 72000)
                assert.eq(lengths.files["voice/act1/alone.wav"], nil)
                verify(check.voiceReport(root, true) :: dialogue.Report)
            end
        end
        -- A spread is not a mistake: the command still answers 0.
        assert.eq(voice.run({ path = root, locale = nil, all = false }), 0)
        fs.removeDirectory(root, { recursive = true })
    end)
end)
