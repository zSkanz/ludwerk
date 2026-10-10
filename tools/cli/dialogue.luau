--!strict
--[[
	A project's dialogue, read as the engine reads it, and what is still to
	write and to record in each language (ADR 0200 §5 and §8).

	**Lines are files**: `content/dialogue/<name>.lines.json`, a table of lines
	by id and of speakers. A line's id in the game is `<name>.<id>`; its text
	is that id in the catalog and its sound is
	`asset://voice/<name>/<id>.ogg`, unless its entry says otherwise. A
	recording in another language is the file of the same name under
	`content/l10n/<locale>/`.

	A game is written before it is recorded, and recorded in one language
	before the others. So most of what this finds is not wrong: a line with no
	recording is played as text, and one not recorded in a language is heard in
	the default one. Two things are wrong, and `ludwerk check` fails on them: a
	file the engine cannot read, and a line with no text in the default
	language, which is a line nobody can read in any.

	The report is a value, built here; `commands/voice.luau` says it and
	`commands/check.luau` sums it up.
]]

local fs = require("@std/fs")
local json = require("@std/json")
local process = require("@std/process")

local catalog = require("./catalog")
local l10n = require("./export/l10n")
local projectlib = require("./project")

local tr = catalog.tr

--- How far apart a line's longest and shortest language may be before it is
--- pointed at, in seconds. A localized sound lasts as long as its longest
--- language (ADR 0200 §2), so the difference is silence after the line in
--- every other one: a third of a second is a pause nobody hears as one, and
--- past it a conversation starts to drag.
local SpreadLimit = 0.35

local LinesEnding = ".lines.json"
local AssetScheme = "asset://"

export type Line = {
    --- `<name>.<id>`: what a game says to play it.
    id: string,
    --- The lines file, relative to the content directory.
    file: string,
    --- The catalog key of what is said.
    text: string,
    --- The sound's file, relative to the content directory; nil for a line
    --- that is never voiced.
    sound: string?,
}

export type Problem = {
    file: string,
    message: string,
}

export type Document = {
    --- How many lines files there are.
    files: number,
    --- Sorted by id.
    lines: { Line },
    --- Every speaker's name key, each once, sorted.
    speakerNames: { string },
    --- What the engine would refuse.
    problems: { Problem },
}

--- A JSON object's own keys, sorted: `@std/json` tags an object with a key
--- that is not a string.
local function keysOf(object: json.Object): { string }
    local keys: { string } = {}
    for key in object do
        if typeof(key) == "string" then
            table.insert(keys, key)
        end
    end
    table.sort(keys)
    return keys
end

--- The lines files under `content/dialogue/`, direct children only, as names
--- without the ending, sorted.
local function lineFiles(content: string): { string }
    local names: { string } = {}
    local folder = projectlib.joined(content, "dialogue")
    if fs.exists(folder) then
        for _, entry in fs.listDirectory(folder) do
            if
                entry.type == "file"
                and #entry.name > #LinesEnding
                and string.sub(entry.name, -#LinesEnding) == LinesEnding
            then
                table.insert(names, string.sub(entry.name, 1, -#LinesEnding - 1))
            end
        end
    end
    table.sort(names)
    return names
end

--- One lines file into `document`. Everything it gets wrong is a problem
--- with the file named, and what it gets right is still read: one bad line
--- does not hide the other four hundred.
local function readFile(content: string, name: string, document: Document, speakerNames: { [string]: boolean })
    local file = `dialogue/{name}{LinesEnding}`
    local function problem(message: string)
        table.insert(document.problems, { file = file, message = message })
    end

    local ok, decoded =
        pcall(json.deserialize, fs.readFileToString(projectlib.joined(content, "dialogue", name .. LinesEnding)))
    local root = if ok then json.asObject(decoded) else nil
    if root == nil then
        problem(tr("cli.voice.err.unreadable", { file = file }))
        return
    end

    local speakers: { [string]: boolean } = {}
    local speakersNode = (root :: json.Object).speakers
    if speakersNode ~= nil then
        local speakerTable = json.asObject(speakersNode)
        if speakerTable == nil then
            problem(tr("cli.voice.err.speakers_shape", { file = file }))
        else
            for _, speaker in keysOf(speakerTable) do
                local entry = json.asObject(speakerTable[speaker])
                local key = if entry ~= nil then entry.name else nil
                if entry == nil or typeof(key) ~= "string" or key == "" then
                    problem(tr("cli.voice.err.speaker_entry", { file = file, speaker = speaker }))
                    -- Still a speaker a line may name: one error for it, not
                    -- one more for every line of theirs.
                    speakers[speaker] = true
                    continue
                end
                if entry.color ~= nil and typeof(entry.color) ~= "string" then
                    problem(tr("cli.voice.err.speaker_color", { file = file, speaker = speaker }))
                end
                speakers[speaker] = true
                speakerNames[key :: string] = true
            end
        end
    end

    local linesNode = (root :: json.Object).lines
    local lines = if linesNode ~= nil then json.asObject(linesNode) else nil
    if lines == nil then
        problem(tr("cli.voice.err.lines_shape", { file = file }))
        return
    end
    for _, id in keysOf(lines) do
        local entry = json.asObject(lines[id])
        if entry == nil then
            problem(tr("cli.voice.err.line_entry", { file = file, line = id }))
            continue
        end
        local wrong = false
        for _, field in { "speaker", "text", "sound" } do
            if entry[field] ~= nil and typeof(entry[field]) ~= "string" then
                problem(tr("cli.voice.err.line_text_field", { file = file, line = id, field = field }))
                wrong = true
            end
        end
        if entry.seconds ~= nil and typeof(entry.seconds) ~= "number" then
            problem(tr("cli.voice.err.line_seconds", { file = file, line = id }))
            wrong = true
        end
        if wrong then
            continue
        end
        local speaker = entry.speaker
        if typeof(speaker) == "string" and not speakers[speaker] then
            problem(tr("cli.voice.err.line_speaker", { file = file, line = id, speaker = speaker }))
        end

        local sound: string? = `voice/{name}/{id}.ogg`
        local named = entry.sound
        if typeof(named) == "string" then
            if named == "" then
                sound = nil
            elseif string.sub(named, 1, #AssetScheme) == AssetScheme and #named > #AssetScheme then
                sound = string.sub(named, #AssetScheme + 1)
            else
                problem(tr("cli.voice.err.line_sound", { file = file, line = id, sound = named }))
                sound = nil
            end
        end
        local text = entry.text
        table.insert(document.lines, {
            id = `{name}.{id}`,
            file = file,
            text = if typeof(text) == "string" and text ~= "" then text else `{name}.{id}`,
            sound = sound,
        })
    end
end

--- Every lines file of `content`, read.
local function read(content: string): Document
    local document: Document = { files = 0, lines = {}, speakerNames = {}, problems = {} }
    local speakerNames: { [string]: boolean } = {}
    for _, name in lineFiles(content) do
        document.files += 1
        readFile(content, name, document, speakerNames)
    end
    table.sort(document.lines, function(a: Line, b: Line): boolean
        return a.id < b.id
    end)
    for key in speakerNames do
        table.insert(document.speakerNames, key)
    end
    table.sort(document.speakerNames)
    return document
end

export type Lengths = {
    --- Frames a second.
    rate: number,
    --- Each measured file, relative to the content directory, in frames.
    files: { [string]: number },
    --- Files that could not be measured.
    unmeasured: { string },
}

--- **Each localized recording's length, from the asset compiler** -- the
--- engine's own measure of the file, which is what an export writes into the
--- game and so the only number worth comparing. Nil when there is no
--- compiler here to ask (a source checkout nobody built), or one from before
--- it could answer.
local function measure(content: string): Lengths?
    local assetc = projectlib.findTool("assetc")
    if assetc == nil or not fs.exists(content) then
        return nil
    end
    local command = { assetc, "l10n-index", "--input", content, "--per-file" }
    -- A program that is not there is an error to the runner, not an answer.
    local ran, listing = pcall(function(): string?
        local asked = process.run(command)
        return if asked.ok then asked.stdout else nil
    end)
    if not ran or listing == nil then
        return nil
    end
    local ok, decoded = pcall(json.deserialize, listing)
    local root = if ok then json.asObject(decoded) else nil
    local files = if root ~= nil then json.asObject(root.files) else nil
    if root == nil or files == nil or typeof(root.rate) ~= "number" or root.rate <= 0 then
        return nil
    end
    local lengths: Lengths = { rate = root.rate :: number, files = {}, unmeasured = {} }
    for _, file in keysOf(files) do
        local frames = files[file]
        if typeof(frames) == "number" then
            lengths.files[file] = frames
        end
    end
    local unmeasured = json.asArray(root.unmeasured)
    if unmeasured ~= nil then
        for _, file in unmeasured do
            if typeof(file) == "string" then
                table.insert(lengths.unmeasured, file)
            end
        end
    end
    return lengths
end

export type Language = {
    locale: string,
    default: boolean,
    --- Whether there is an `i18n/<locale>.json` to read its text from.
    catalog: boolean,
    --- Whether anything at all is recorded in it.
    voiced: boolean,
    --- Lines whose text key its catalog does not have: every line, with no
    --- catalog.
    noText: { string },
    --- Lines recorded in some language and not in this one.
    noRecording: { string },
    --- Sound files under its `voice/` that no line names, relative to the
    --- content directory.
    unusedSounds: { string },
    --- Speakers' name keys its catalog does not have. Empty with no catalog:
    --- that is said once, by `catalog`.
    noSpeakerName: { string },
    --- Its own recordings of lines that have no viseme track beside them
    --- (ADR 0200 §9), relative to the content directory: a mouth is moved by
    --- the sound alone for those.
    noTrack: { string },
}

export type Spread = {
    line: string,
    --- Seconds, and the language each is.
    shortest: number,
    shortestLocale: string,
    longest: number,
    longestLocale: string,
    spread: number,
    --- Further apart than `SpreadLimit`.
    flagged: boolean,
}

export type Report = {
    defaultLocale: string,
    files: number,
    lines: number,
    --- The default first, then by name.
    languages: { Language },
    --- Lines with no recording in any language: played as text.
    silent: { string },
    problems: { Problem },
    --- False when the recordings' lengths could not be asked for.
    measured: boolean,
    --- Lines recorded in more than one language, furthest apart first.
    spreads: { Spread },
    flagged: number,
    unmeasured: { string },
}

--- `pt` for `pt-BR`: where a name is looked for after its own language's
--- folder and before the default (ADR 0200 §1). Nil for a bare language.
local function parentLanguage(locale: string): string?
    local parent = string.match(locale, "^([^%-_]+)[%-_]")
    return if parent ~= nil and parent ~= locale then parent else nil
end

--- The catalogs of a project, by locale: each one's keys. A file that is not
--- a catalog is left out; `check` says so itself.
local function readCatalogs(
    root: string,
    catalogKeys: (string) -> { [string]: boolean }?
): { [string]: { [string]: boolean } }
    local catalogs: { [string]: { [string]: boolean } } = {}
    local folder = projectlib.joined(root, "i18n")
    if fs.exists(folder) then
        for _, entry in fs.listDirectory(folder) do
            if entry.type == "file" and string.sub(entry.name, -5) == ".json" then
                local keys = catalogKeys(projectlib.joined(folder, entry.name))
                if keys ~= nil then
                    catalogs[string.sub(entry.name, 1, -6)] = keys
                end
            end
        end
    end
    return catalogs
end

export type Options = {
    --- The project's folder, where `i18n/` is.
    root: string,
    --- Its content directory.
    content: string,
    defaultLocale: string,
    --- How a catalog file is read: `check`'s own reader, handed in so that
    --- there is one.
    catalogKeys: (string) -> { [string]: boolean }?,
    --- The recordings' lengths, or nil when they were not measured.
    lengths: Lengths?,
}

--- **The report**: for the default language, every catalog and every folder
--- under `l10n/`, what each line still lacks. Nil for a project with no lines
--- file, which none of this is about.
local function report(options: Options): Report?
    local content = options.content
    local document = read(content)
    if document.files == 0 then
        return nil
    end
    local first = options.defaultLocale
    local catalogs = readCatalogs(options.root, options.catalogKeys)
    local folders = l10n.folders(content)

    local locales: { string } = {}
    local seen: { [string]: boolean } = { [first] = true }
    for locale in catalogs do
        if not seen[locale] then
            seen[locale] = true
            table.insert(locales, locale)
        end
    end
    for _, locale in folders do
        if not seen[locale] then
            seen[locale] = true
            table.insert(locales, locale)
        end
    end
    table.sort(locales)
    table.insert(locales, 1, first)

    --- Whether `locale` has its own recording of `sound`: in its folder, or
    --- in its language's. The default's is the plain file.
    local function recorded(locale: string, sound: string): boolean
        if locale == first and fs.exists(projectlib.joined(content, sound)) then
            return true
        end
        if fs.exists(projectlib.joined(content, "l10n", locale, sound)) then
            return true
        end
        local parent = parentLanguage(locale)
        return parent ~= nil and fs.exists(projectlib.joined(content, "l10n", parent, sound))
    end

    -- Each voiced line: is it recorded anywhere at all?
    local anywhere: { [string]: boolean } = {}
    local named: { [string]: boolean } = {}
    local silent: { string } = {}
    for _, line in document.lines do
        local sound = line.sound
        if sound == nil then
            continue
        end
        named[sound] = true
        local found = fs.exists(projectlib.joined(content, sound))
        for _, locale in folders do
            found = found or fs.exists(projectlib.joined(content, "l10n", locale, sound))
        end
        anywhere[line.id] = found
        if not found then
            table.insert(silent, line.id)
        end
    end

    local languages: { Language } = {}
    for _, locale in locales do
        local keys = catalogs[locale]
        local voiceFolder = if locale == first
            then projectlib.joined(content, "voice")
            else projectlib.joined(content, "l10n", locale, "voice")
        local prefix = if locale == first then "voice/" else `l10n/{locale}/voice/`
        local parent = parentLanguage(locale)
        local language: Language = {
            locale = locale,
            default = locale == first,
            catalog = keys ~= nil,
            voiced = locale == first
                or #l10n.soundsUnder(projectlib.joined(content, "l10n", locale)) > 0
                or (parent ~= nil and #l10n.soundsUnder(projectlib.joined(content, "l10n", parent)) > 0),
            noText = {},
            noRecording = {},
            unusedSounds = {},
            noSpeakerName = {},
            noTrack = {},
        }
        for _, line in document.lines do
            if keys == nil or not keys[line.text] then
                table.insert(language.noText, line.id)
            end
            local sound = line.sound
            if sound ~= nil and anywhere[line.id] and not recorded(locale, sound) then
                table.insert(language.noRecording, line.id)
            end
            -- This language's OWN recording, and whether a track is beside it:
            -- the same name with `.visemes.json` where its extension was.
            if sound ~= nil then
                local own = if locale == first then sound else `l10n/{locale}/{sound}`
                if fs.exists(projectlib.joined(content, own)) then
                    local track = (string.gsub(own, "%.[^./]*$", "")) .. ".visemes.json"
                    if not fs.exists(projectlib.joined(content, track)) then
                        table.insert(language.noTrack, own)
                    end
                end
            end
        end
        for _, relative in l10n.soundsUnder(voiceFolder) do
            if not named[`voice/{relative}`] then
                table.insert(language.unusedSounds, prefix .. relative)
            end
        end
        if keys ~= nil then
            for _, key in document.speakerNames do
                if not keys[key] then
                    table.insert(language.noSpeakerName, key)
                end
            end
        end
        table.insert(languages, language)
    end

    local spreads: { Spread } = {}
    local flagged = 0
    local lengths = options.lengths
    if lengths ~= nil then
        for _, line in document.lines do
            local sound = line.sound
            if sound == nil then
                continue
            end
            -- Each language's own file, the default's first: a language that
            -- only borrows another's recording is not a length of its own.
            local measured: { { locale: string, seconds: number } } = {}
            local own = lengths.files[sound]
            if own ~= nil then
                table.insert(measured, { locale = first, seconds = own / lengths.rate })
            end
            for _, locale in folders do
                local frames = lengths.files[`l10n/{locale}/{sound}`]
                if frames ~= nil then
                    table.insert(measured, { locale = locale, seconds = frames / lengths.rate })
                end
            end
            if #measured < 2 then
                continue
            end
            local shortest = measured[1]
            local longest = measured[1]
            for _, entry in measured do
                if entry.seconds < shortest.seconds then
                    shortest = entry
                end
                if entry.seconds > longest.seconds then
                    longest = entry
                end
            end
            local spread = longest.seconds - shortest.seconds
            if spread > SpreadLimit then
                flagged += 1
            end
            table.insert(spreads, {
                line = line.id,
                shortest = shortest.seconds,
                shortestLocale = shortest.locale,
                longest = longest.seconds,
                longestLocale = longest.locale,
                spread = spread,
                flagged = spread > SpreadLimit,
            })
        end
        table.sort(spreads, function(a: Spread, b: Spread): boolean
            if a.spread ~= b.spread then
                return a.spread > b.spread
            end
            return a.line < b.line
        end)
    end

    return {
        defaultLocale = first,
        files = document.files,
        lines = #document.lines,
        languages = languages,
        silent = silent,
        problems = document.problems,
        measured = lengths ~= nil,
        spreads = spreads,
        flagged = flagged,
        unmeasured = if lengths ~= nil then lengths.unmeasured else {},
    }
end

export type Totals = {
    --- Lines with no text in the default language: what fails a check.
    untranslatedDefault: number,
    --- Lines with no text in some language, the default among them.
    untranslated: number,
    --- Lines some language has no recording of.
    unrecorded: number,
    unused: number,
    speakerNames: number,
}

--- The report as counts, each line counted once however many languages it is
--- missing from.
local function totals(found: Report): Totals
    local sums: Totals = { untranslatedDefault = 0, untranslated = 0, unrecorded = 0, unused = 0, speakerNames = 0 }
    local untranslated: { [string]: boolean } = {}
    local unrecorded: { [string]: boolean } = {}
    for _, language in found.languages do
        if language.default then
            sums.untranslatedDefault = #language.noText
        end
        for _, line in language.noText do
            untranslated[line] = true
        end
        for _, line in language.noRecording do
            unrecorded[line] = true
        end
        sums.unused += #language.unusedSounds
        sums.speakerNames += #language.noSpeakerName
    end
    for _ in untranslated do
        sums.untranslated += 1
    end
    for _ in unrecorded do
        sums.unrecorded += 1
    end
    return sums
end

return {
    SpreadLimit = SpreadLimit,
    lineFiles = lineFiles,
    read = read,
    measure = measure,
    report = report,
    totals = totals,
    parentLanguage = parentLanguage,
}
