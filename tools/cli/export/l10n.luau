--!strict
--[[
	The voices an export ships apart from the game (ADR 0200 §7).

	A localized sound is the file of the same name under
	`content/l10n/<locale>/`. `[export] voice = ["en", "pt-BR"]` names the
	voice languages whose sounds go in the game's own pack; absent, all do.
	Every other language's SOUNDS are left out of it and packed into a pack of
	their own, `l10n-<locale>.lpack`, that holds nothing but the names
	`asset://l10n/<locale>/...`. Pictures under `l10n/` stay in the game's pack
	in every language: they are small.

	**The index is made before anything is taken out** -- `l10n/index.json` in
	the staged content, by `assetc l10n-index` -- because it holds each
	localized sound's longest length among ALL the languages, and a machine
	with no language pack installed still has to end a line on the same tick
	as one that has (ADR 0200 §2).

	Everything here works on the STAGED copy of `content/`. A staged file is a
	hard link to the project's own, so one is never written through: it is
	removed, and a file is made in its place.
]]

local fs = require("@std/fs")
local process = require("@std/process")

local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

--- What under `l10n/<locale>/` is a voice, by its ending in any case.
--- Everything else there is a localized picture.
local SoundEndings = { ".ogg", ".wav", ".mp3", ".flac" }

local function isSound(name: string): boolean
    local lowered = string.lower(name)
    for _, ending in SoundEndings do
        if #lowered > #ending and string.sub(lowered, -#ending) == ending then
            return true
        end
    end
    return false
end

--- Every sound file under `folder`, relative to it with `/`, sorted.
local function soundsUnder(folder: string): { string }
    local found: { string } = {}
    local function walk(directory: string, relative: string)
        for _, entry in fs.listDirectory(directory) do
            local name = if relative == "" then entry.name else `{relative}/{entry.name}`
            if entry.type == "dir" then
                walk(projectlib.joined(directory, entry.name), name)
            elseif entry.type == "file" and isSound(entry.name) then
                table.insert(found, name)
            end
        end
    end
    if fs.exists(folder) then
        walk(folder, "")
    end
    table.sort(found)
    return found
end

--- The folders under `content/l10n/`, sorted: every language the project has
--- anything in.
local function folders(content: string): { string }
    local names: { string } = {}
    local root = projectlib.joined(content, "l10n")
    if fs.exists(root) then
        for _, entry in fs.listDirectory(root) do
            if entry.type == "dir" then
                table.insert(names, entry.name)
            end
        end
    end
    table.sort(names)
    return names
end

--- The voice languages: the folders that hold a sound file at any depth.
local function voiceLanguages(content: string): { string }
    local names: { string } = {}
    for _, locale in folders(content) do
        if #soundsUnder(projectlib.joined(content, "l10n", locale)) > 0 then
            table.insert(names, locale)
        end
    end
    return names
end

--- Whether `content` has anything the index describes: a folder of languages,
--- or a lines file. A project with neither is exported as it always was.
local function hasAnything(content: string): boolean
    if #folders(content) > 0 then
        return true
    end
    local dialogue = projectlib.joined(content, "dialogue")
    if fs.exists(dialogue) then
        for _, entry in fs.listDirectory(dialogue) do
            if entry.type == "file" and #entry.name > 11 and string.sub(entry.name, -11) == ".lines.json" then
                return true
            end
        end
    end
    return false
end

export type Plan = {
    --- Every voice language of the project, sorted. Not the default: its
    --- sounds are the plain files.
    voice: { string },
    --- Those whose sounds are in the game's own pack.
    shipped: { string },
    --- Those that get a pack of their own.
    apart: { string },
    --- What `[export] voice` would have left out, on a target where every
    --- language ships whatever it says.
    overruled: { string },
}

--- Which voice languages go where. `everyLanguageShips` is a target that has
--- no language packs -- Android -- and ships all of them inside the game.
---
--- A name in `[export] voice` that is neither a folder under `l10n/` nor the
--- default locale is an error: a language spelt wrong would otherwise be a
--- language left out of the game without a word.
local function plan(
    project: projectlib.Project,
    content: string,
    defaultLocale: string,
    everyLanguageShips: boolean
): (Plan?, string?)
    local voice = voiceLanguages(content)
    local section = project.config.export
    local named = if typeof(section) == "table" then section.voice else nil
    if named == nil then
        return { voice = voice, shipped = voice, apart = {}, overruled = {} }, nil
    end
    if typeof(named) ~= "table" then
        return nil, tr("cli.build.err.voice_value")
    end
    local known: { [string]: boolean } = {}
    for _, locale in folders(content) do
        known[locale] = true
    end
    local wanted: { [string]: boolean } = {}
    for _, name in named :: { any } do
        if typeof(name) ~= "string" then
            return nil, tr("cli.build.err.voice_value")
        end
        if not known[name] and name ~= defaultLocale then
            return nil, tr("cli.build.err.voice_unknown", { locale = name, default = defaultLocale })
        end
        wanted[name] = true
    end
    local shipped: { string } = {}
    local apart: { string } = {}
    for _, locale in voice do
        table.insert(if wanted[locale] then shipped else apart, locale)
    end
    if everyLanguageShips then
        return { voice = voice, shipped = voice, apart = {}, overruled = apart }, nil
    end
    return { voice = voice, shipped = shipped, apart = apart, overruled = {} }, nil
end

--- Writes `l10n/index.json` into the staged content, with `shipped` as the
--- languages that stay in the game's pack. Nothing is written for content
--- with no voice language and no lines file. Returns what went wrong.
local function writeIndex(assetc: string, staged: string, shipped: { string }): string?
    local target = projectlib.joined(staged, "l10n", "index.json")
    -- A project's own file of that name is not the index, and its staged copy
    -- is a link to it: unlinked, never written over.
    if fs.exists(target) then
        fs.remove(target)
    end
    -- A lone comma is a list with no name in it: an empty argument does not
    -- survive every shell on the way to the compiler.
    local list = if #shipped == 0 then "," else table.concat(shipped, ",")
    local made = process.run({ assetc, "l10n-index", "--input", staged, "--output", target, "--shipped", list })
    if not made.ok then
        return made.stdout .. made.stderr .. "\n" .. tr("cli.build.err.voice_index", { compiler = assetc })
    end
    return nil
end

--- Takes `locale`'s sound files out of the staged content and puts them
--- under `into` at the paths they had, so that packing `into` gives them the
--- names they had: `asset://l10n/<locale>/...`. Returns how many.
local function takeApart(staged: string, locale: string, into: string): number
    local from = projectlib.joined(staged, "l10n", locale)
    local to = projectlib.joined(into, "l10n", locale)
    local moved = 0
    for _, relative in soundsUnder(from) do
        local source = projectlib.joined(from, relative)
        local target = projectlib.joined(to, relative)
        local folder = string.match(target, "^(.*)[/\\][^/\\]+$")
        if folder ~= nil then
            fs.createDirectory(folder, { makeParents = true })
        end
        if not pcall(fs.link, source, target) then
            fs.copy(source, target)
        end
        fs.remove(source)
        moved += 1
    end
    return moved
end

--- Packs the content under `stage` into `pack`, and seals it when the game
--- itself is sealed (ADR 0183): a language pack is the same kind of container
--- as the pack it is mounted after. Sealed, it is one file; plain, it is the
--- pack and the manifest beside it. Returns what went wrong.
local function buildPack(assetc: string, stage: string, pack: string, sealed: boolean): string?
    local manifest = string.gsub(pack, "%.lpack$", "") .. ".manifest.json"
    local built = process.run({ assetc, "--input", stage, "--output", pack, "--manifest", manifest })
    if not built.ok then
        return built.stdout .. built.stderr .. "\n" .. tr("cli.assets.err.failed")
    end
    if sealed then
        local done = process.run({ assetc, "seal-pack", pack })
        if not done.ok then
            return done.stdout .. done.stderr .. "\n" .. tr("cli.build.err.seal_failed", { compiler = assetc })
        end
    end
    return nil
end

--- The files of one language's pack: the pack, and its manifest when the
--- game is not sealed.
local function packFiles(locale: string, sealed: boolean): { string }
    if sealed then
        return { `l10n-{locale}.lpack` }
    end
    return { `l10n-{locale}.lpack`, `l10n-{locale}.manifest.json` }
end

--- Whether `name` is a file this module writes into a `languages/` folder.
local function isPackFile(name: string): boolean
    return string.match(name, "^l10n%-.+%.lpack$") ~= nil or string.match(name, "^l10n%-.+%.manifest%.json$") ~= nil
end

--- Copies the packs of `locales` from `built` into `folder`, and removes from
--- it every language pack that is not one of them: a language that ships
--- inside the game now must not leave last build's pack beside it. Only
--- files named as a language pack is are touched: the folder is beside the
--- export, where anything else may be.
local function publish(built: string, folder: string, locales: { string }, sealed: boolean)
    local wanted: { [string]: boolean } = {}
    for _, locale in locales do
        for _, file in packFiles(locale, sealed) do
            wanted[file] = true
        end
    end
    if fs.exists(folder) then
        for _, entry in fs.listDirectory(folder) do
            if entry.type == "file" and isPackFile(entry.name) and not wanted[entry.name] then
                fs.remove(projectlib.joined(folder, entry.name))
            end
        end
    end
    if #locales == 0 then
        return
    end
    fs.createDirectory(folder, { makeParents = true })
    for file in wanted do
        local target = projectlib.joined(folder, file)
        if fs.exists(target) then
            fs.remove(target)
        end
        fs.copy(projectlib.joined(built, file), target)
    end
end

return {
    isSound = isSound,
    soundsUnder = soundsUnder,
    folders = folders,
    voiceLanguages = voiceLanguages,
    hasAnything = hasAnything,
    plan = plan,
    writeIndex = writeIndex,
    takeApart = takeApart,
    buildPack = buildPack,
    packFiles = packFiles,
    publish = publish,
}
