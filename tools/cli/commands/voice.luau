--!strict
--[[
	`ludwerk voice [path] [--locale=<locale>] [--all]` (ADR 0200 §8).

	What a game's dialogue still lacks, a language: the lines with no text
	(read in the default language's), the lines with no recording (heard in
	the default language), and the sound files under its `voice/` that no line
	names. Once, for all of them: the lines recorded in no language, which are
	played as text; and the lines whose languages are of different lengths,
	furthest apart first -- a localized sound lasts as long as its longest
	language (ADR 0200 §2), so one long recording is silence after the line in
	every other language, and nothing else shows it.

	Most of it is work still to do, not a mistake, and the command answers 0.
	It answers 1 for what the engine would refuse: a lines file that does not
	parse, and a line naming a speaker its file does not have.

	The lengths are the asset compiler's, which measures a file as the engine
	does. Where there is none -- a source checkout nobody built -- everything
	else is still said, and that is said once.
]]

local fs = require("@std/fs")
local platform = require("@std/system/platform")
local process = require("@std/process")

local catalog = require("../catalog")
local check = require("./check")
local dialogue = require("../dialogue")
local projectlib = require("../project")

local tr = catalog.tr

export type Options = {
    path: string,
    --- One language, where the default is every language of the project.
    locale: string?,
    --- Every item of every list, where the default stops a list at `Cap`.
    all: boolean,
    --- Make the viseme tracks that are missing, with the lip-sync tool the
    --- developer has installed (ADR 0200 §9). The engine reads a track and
    --- never makes one; this runs what does.
    visemes: boolean?,
}

--- How many items of a list are said before "and N more". A game of five
--- hundred lines with nothing recorded yet is five hundred lines a section,
--- and the count is what a person reads first.
local Cap = 20

--- `items`, one a line, up to the cap.
local function list(items: { string }, all: boolean)
    for index, item in items do
        if not all and index > Cap then
            print(tr("cli.voice.more", { count = #items - Cap }))
            return
        end
        print(tr("cli.voice.item", { item = item }))
    end
end

local function seconds(value: number): string
    return string.format("%.2f", value)
end

local function sayLanguage(language: dialogue.Language, found: dialogue.Report, all: boolean)
    local first = found.defaultLocale
    -- Each key named whole, never chosen inside the call: the i18n lint reads
    -- the literal after `tr(`.
    if language.default then
        print(tr("cli.voice.language_default", { locale = language.locale }))
    else
        print(tr("cli.voice.language", { locale = language.locale }))
    end

    -- (a) The text.
    if not language.catalog then
        print(tr("cli.voice.no_catalog", { locale = language.locale, count = #language.noText, default = first }))
    elseif language.default then
        print(tr("cli.voice.no_text_default", { count = #language.noText }))
        list(language.noText, all)
    else
        print(tr("cli.voice.no_text", { count = #language.noText, default = first }))
        list(language.noText, all)
    end

    -- (b) The recordings.
    if not language.voiced then
        print(tr("cli.voice.no_voice", { locale = language.locale, count = #language.noRecording, default = first }))
    elseif language.default then
        print(tr("cli.voice.no_recording_default", { count = #language.noRecording }))
        list(language.noRecording, all)
    else
        print(tr("cli.voice.no_recording", { count = #language.noRecording, default = first }))
        list(language.noRecording, all)
    end

    -- (d) Recordings nothing plays.
    print(tr("cli.voice.unused", { count = #language.unusedSounds }))
    list(language.unusedSounds, all)

    -- (g) Recordings a mouth has no track for.
    if language.voiced then
        print(tr("cli.voice.no_track", { count = #language.noTrack }))
        list(language.noTrack, all)
    end

    -- (e) Who is speaking, in this language.
    if language.catalog then
        print(tr("cli.voice.no_speaker", { count = #language.noSpeakerName, locale = language.locale }))
        list(language.noSpeakerName, all)
    end
end

--- **The lip-sync tool on this machine, or nil.** Located, never bundled and
--- never fetched -- the rule for anything that is not the engine's own (ADR
--- 0188): the developer installed it, and `ENG_LIPSYNC_TOOL` or the PATH says
--- where. It is the common free one, whose executable is `rhubarb`; anything
--- that takes its arguments and writes its file serves.
local function lipSyncTool(): string?
    local named = process.env.ENG_LIPSYNC_TOOL
    if named ~= nil and named ~= "" then
        return if fs.exists(named) then named else nil
    end
    local asked = process.run({ if platform.win32 then "where" else "which", "rhubarb" })
    if not asked.ok then
        return nil
    end
    local first = string.match(asked.stdout, "[^\r\n]+")
    return if first ~= nil and first ~= "" then first else nil
end

--- Makes the track of every recording that has none, a language at a time.
--- How many were made and how many the tool refused.
local function makeTracks(content: string, report: dialogue.Report, wanted: string?): (number, number)
    local tool = lipSyncTool()
    if tool == nil then
        print(tr("cli.voice.visemes_no_tool"))
        return 0, 0
    end
    local made, failed = 0, 0
    for _, language in report.languages do
        if wanted ~= nil and language.locale ~= wanted then
            continue
        end
        for _, recording in language.noTrack do
            local sound = projectlib.joined(content, recording)
            local track = projectlib.joined(content, (string.gsub(recording, "%.[^./]*$", "")) .. ".visemes.json")
            local ran = process.run({ tool :: string, "-f", "json", "-o", track, sound })
            if ran.ok and fs.exists(track) then
                made += 1
                print(tr("cli.voice.visemes_made", { file = recording }))
            else
                failed += 1
                print(tr("cli.voice.visemes_failed", { file = recording }))
            end
        end
    end
    print(tr("cli.voice.visemes_done", { made = made, failed = failed }))
    return made, failed
end

local function run(options: Options): number
    local project = projectlib.load(options.path)
    if project.diagnostic ~= nil then
        print(project.diagnostic :: string)
        return 1
    end
    local found = check.voiceReport(project.root, true)
    -- Made first, so the report that follows is of what there is now.
    if found ~= nil and options.visemes then
        local _, failed =
            makeTracks(projectlib.joined(project.root, "content"), found :: dialogue.Report, options.locale)
        found = check.voiceReport(project.root, true)
        if failed > 0 and found == nil then
            return 1
        end
    end
    if found == nil then
        print(tr("cli.voice.none", { path = project.root }))
        return 0
    end
    local report = found :: dialogue.Report

    local wanted = options.locale
    if wanted ~= nil then
        local known: { string } = {}
        local has = false
        for _, language in report.languages do
            table.insert(known, language.locale)
            has = has or language.locale == wanted
        end
        if not has then
            print(tr("cli.voice.err.unknown_locale", { locale = wanted, locales = table.concat(known, ", ") }))
            return 2
        end
    end

    print(tr("cli.voice.header", { lines = report.lines, files = report.files, locale = report.defaultLocale }))
    for _, language in report.languages do
        if wanted == nil or language.locale == wanted then
            sayLanguage(language, report, options.all)
        end
    end

    -- (c) Once: it is the same list in every language.
    print(tr("cli.voice.silent", { count = #report.silent }))
    list(report.silent, options.all)

    -- The lengths, furthest apart first.
    if not report.measured then
        print(tr("cli.voice.not_measured"))
    else
        print(tr("cli.voice.spreads", {
            count = #report.spreads,
            flagged = report.flagged,
            limit = seconds(dialogue.SpreadLimit),
        }))
        for index, spread in report.spreads do
            if not options.all and index > Cap then
                print(tr("cli.voice.more", { count = #report.spreads - Cap }))
                break
            end
            local said = {
                line = spread.line,
                shortest = seconds(spread.shortest),
                shortestLocale = spread.shortestLocale,
                longest = seconds(spread.longest),
                longestLocale = spread.longestLocale,
                spread = seconds(spread.spread),
            }
            if spread.flagged then
                print(tr("cli.voice.spread_flagged", said))
            else
                print(tr("cli.voice.spread", said))
            end
        end
        if #report.unmeasured > 0 then
            print(tr("cli.voice.unmeasured", { count = #report.unmeasured }))
            list(report.unmeasured, options.all)
        end
    end

    -- (f) What the engine would refuse.
    if #report.problems > 0 then
        print(tr("cli.voice.errors", { count = #report.problems }))
        for _, problem in report.problems do
            print(tr("cli.voice.item", { item = problem.message }))
        end
    end

    local sums = dialogue.totals(report)
    print(tr("cli.voice.summary", {
        lines = report.lines,
        untranslated = sums.untranslated,
        unrecorded = sums.unrecorded,
        silent = #report.silent,
        unused = sums.unused,
        flagged = if report.measured then tostring(report.flagged) else tr("cli.voice.unknown"),
        limit = seconds(dialogue.SpreadLimit),
        errors = #report.problems,
    }))
    return if #report.problems > 0 then 1 else 0
end

return {
    run = run,
    Cap = Cap,
}
