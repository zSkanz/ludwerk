--!strict
--[[
	`ludwerk check` — the one implementation of "is this tree clean".

	M3 brief, Decision 7: this is the implementation and
	`scripts/gates/luau-check.sh` calls it, rather than the two drifting until
	they disagree about whether the repository is green. Two checks, in the
	order that gives the most useful first failure: types, then formatting.
	Then **where each script runs** (ADR 0138 §8): warnings about a script's
	side, in its `.luau` file or inside a scene, a stamp or `global.json`,
	from the engine's own lint (`engine-host --check-sides`) so that the editor
	and this say the same thing. Warnings do not fail the check.

	**The i18n lint is not one of them** (S0.8): it checks the engine's own
	`ENG_TR` keys against `i18n/en.json`, which a game does not have, so it is
	the repository's -- `tools/repo/i18nlint.luau`, which the gate runs beside
	this. The header said three until 2026-09-29, and `run` never had it.

	**A game's own catalogs are** (ADR 0154): every key of the default
	locale's `i18n/<locale>.json` is in every other, or the check fails and
	names each one that is not -- a missing translation is shown to a player
	as another language's text. A key only another locale has is said and does
	not fail: it is text nobody asks for.
]]

local fs = require("@std/fs")
local json = require("@std/json")
local process = require("@std/process")

local catalog = require("../catalog")
local projectlib = require("../project")
local tools = require("../tools")

local tr = catalog.tr

export type Options = {
    path: string,
    -- Extra `.d.luau` files to load. The repository passes its generated engine
    -- definitions; a scaffolded project passes `.engine/types/engine.d.luau`.
    definitions: { string },
    -- Directories to skip. Vendored trees and Lute's own typedefs, which do not
    -- typecheck under the pinned luau-lsp (M0 finding 2).
    ignore: { string },
}

local function collectLuau(root: string, ignore: { string }): { string }
    local files: { string } = {}
    local pending: { string } = { root }

    -- A whole folder name, never a substring: `dist` ignores `dist/` and not
    -- `src/distance.luau` (D211).
    local function ignored(candidate: string): boolean
        for _, segment in string.split(string.gsub(candidate, "\\", "/"), "/") do
            if table.find(ignore, segment) ~= nil then
                return true
            end
        end
        return false
    end

    while #pending > 0 do
        local dir = table.remove(pending) :: string
        for _, entry in fs.listDirectory(dir) do
            local child = projectlib.joined(dir, entry.name)
            if ignored(child) then
                continue
            end
            if entry.type == "dir" then
                table.insert(pending, child)
            elseif entry.type == "file" and string.sub(entry.name, -5) == ".luau" then
                -- A `.d.luau` declares; it is not a source, and analysing it as
                -- one is a page of syntax errors on a perfectly valid file.
                -- They go in through `--definitions` instead.
                if string.sub(entry.name, -7) ~= ".d.luau" then
                    table.insert(files, child)
                end
            end
        end
    end

    table.sort(files)
    return files
end

--- **The definitions of an engine before 2026-10-06, as the new solver can
--- read them** (R2, D192). The generator writes them that way itself now, and
--- this returns such a file as it is; a project whose `.engine/types/` an
--- older engine wrote, or a file somebody passes by hand, is still rewritten.
--- That file typed `Instance.new` as an intersection of one overload per
--- creatable class, which the old solver reads and the new one gives up on as
--- "code too complex" -- so every `Instance.new("Part")` became an `Instance`.
--- The new solver has what the intersection spelled out: a table from name to
--- class, read with `index<>` over a name constrained to `keyof<>` of it, so a
--- literal keeps its name and a wrong one is an error.
---
--- A copy, not an edit: the generated file stays readable by the old solver,
--- which is what an editor extension left on its default still runs, and the
--- engine's own checker reads it with a magic function instead (ADR 0093).
--- Returns the path to analyse with, which is the original when there is no
--- intersection to rewrite.
local function forNewSolver(definition: string, scratch: string): string
    local text = fs.readFileToString(definition)
    local lines = string.split(text, "\n")
    local first: number? = nil
    for index, line in lines do
        if line == "declare Instance: {" and string.sub(lines[index + 1] or "", 1, 12) == '    new: (("' then
            first = index + 1
            break
        end
    end
    if first == nil then
        return definition
    end
    local last = first
    while string.sub(lines[last + 1] or "", 1, 13) == '        & (("' do
        last += 1
    end
    local entries: { string } = {}
    for index = first, last do
        for name, class in string.gmatch(lines[index], '%(%("(%w+)"%) %-> (%w+)%)') do
            table.insert(entries, `    {name}: {class},`)
        end
    end
    local out: { string } = {}
    for index = 1, first - 2 do
        table.insert(out, lines[index])
    end
    table.insert(out, "type InstanceClasses = {")
    for _, entry in entries do
        table.insert(out, entry)
    end
    table.insert(out, "}")
    table.insert(out, "")
    table.insert(out, "declare Instance: {")
    table.insert(out, "    new: <K>(className: K & keyof<InstanceClasses>) -> index<InstanceClasses, K>,")
    for index = last + 1, #lines do
        table.insert(out, lines[index])
    end
    if not fs.exists(scratch) then
        fs.createDirectory(scratch, { makeParents = true })
    end
    local target = projectlib.joined(scratch, "engine.d.luau")
    fs.writeStringToFile(target, table.concat(out, "\n"))
    return target
end

--- `@engine` for `engine.d.luau`: what the analyser calls a definitions file
--- when it says it loaded one.
local function definitionName(path: string): string
    local file = string.match(string.gsub(path, "\\", "/"), "([^/]+)$") or path
    return string.match(file, "^(.-)%.d%.luau$") or string.match(file, "^(.-)%.luau$") or file
end

--- StyLua over the project minus the ignored folders, which it would otherwise
--- walk into -- `dist/` above all (D211). Globs rather than a file list, which
--- would outgrow a Windows command line on a large project.
local function styluaCommand(prefix: { string }, path: string, ignore: { string }): { string }
    local command = table.clone(prefix)
    table.insert(command, "--glob=**/*.luau")
    table.insert(command, "--glob=**/*.lua")
    -- Declarations are not sources (see `collectLuau`), and an explicit glob
    -- overrides a project's `.styluaignore`, which is where they were skipped.
    table.insert(command, "--glob=!**/*.d.luau")
    for _, folder in ignore do
        table.insert(command, `--glob=!**/{folder}/**`)
    end
    table.insert(command, path)
    return command
end

--- The keys of one catalog file, or nil for a file that is not a flat object.
local function catalogKeys(file: string): { [string]: boolean }?
    local ok, decoded = pcall(json.deserialize, fs.readFileToString(file))
    local root = if ok then json.asObject(decoded) else nil
    if root == nil then
        return nil
    end
    local keys: { [string]: boolean } = {}
    for key in root :: json.Object do
        -- `@std/json` tags a decoded object with a sentinel key that is not a
        -- string, and `$`-prefixed entries are a file's own notes.
        if typeof(key) == "string" and string.sub(key, 1, 1) ~= "$" then
            keys[key] = true
        end
    end
    return keys
end

--- A project's default locale: `[project] default_locale`, `en` until it says.
local function defaultLocale(root: string): string
    local file = projectlib.joined(root, "project.toml")
    if not fs.exists(file) then
        return "en"
    end
    local named = string.match(fs.readFileToString(file), '\n%s*default_locale%s*=%s*"([%w_%-]+)"')
    return named or "en"
end

export type CatalogReport = {
    --- Each `{ locale, key }` the default locale has and that locale does not.
    missing: { { locale: string, key: string } },
    --- Each `{ locale, key }` a locale has and the default does not.
    extra: { { locale: string, key: string } },
    --- Each file that is not a catalog.
    unreadable: { string },
    --- How many catalogs were compared: none when the project has no `i18n`.
    catalogs: number,
}

--- The keys of the engine's own catalog, beside the host that was found, or
--- nothing when it is not there to read.
local function engineCatalogKeys(engine: string?): { [string]: boolean }?
    if engine == nil then
        return nil
    end
    local folder = string.match(engine, "^(.*)[/\\][^/\\]*$")
    if folder == nil then
        return nil
    end
    local file = projectlib.joined(folder, "content", "i18n", "en.json")
    if not fs.exists(file) then
        return nil
    end
    return catalogKeys(file)
end

--- A project's catalogs against its default locale's (ADR 0154), in a fixed
--- order: by locale, then by key.
---
--- **A key of the engine's own is not a stray one**: a game re-words the
--- engine's text, or says it in a language the engine has no catalog for, by
--- putting the engine's key in its own catalog -- in one locale and not in
--- another, as it likes. `engineKeys` is the engine's catalog when it could be
--- read; without it, a key under `engine.` is taken for the engine's.
local function checkCatalogs(root: string, engineKeys: { [string]: boolean }?): CatalogReport
    local report: CatalogReport = { missing = {}, extra = {}, unreadable = {}, catalogs = 0 }
    local folder = projectlib.joined(root, "i18n")
    if not fs.exists(folder) then
        return report
    end
    local names: { string } = {}
    for _, entry in fs.listDirectory(folder) do
        if entry.type == "file" and string.sub(entry.name, -5) == ".json" then
            table.insert(names, string.sub(entry.name, 1, -6))
        end
    end
    table.sort(names)

    local catalogs: { [string]: { [string]: boolean } } = {}
    for _, name in names do
        local keys = catalogKeys(projectlib.joined(folder, `{name}.json`))
        if keys == nil then
            table.insert(report.unreadable, `i18n/{name}.json`)
        else
            catalogs[name] = keys
            report.catalogs += 1
        end
    end

    local first = defaultLocale(root)
    local reference = catalogs[first]
    if reference == nil then
        return report
    end
    local referenceKeys: { string } = {}
    for key in reference do
        table.insert(referenceKeys, key)
    end
    table.sort(referenceKeys)
    for _, name in names do
        local keys = catalogs[name]
        if name == first or keys == nil then
            continue
        end
        for _, key in referenceKeys do
            if not keys[key] then
                table.insert(report.missing, { locale = name, key = key })
            end
        end
        local own: { string } = {}
        for key in keys do
            local engines = if engineKeys then engineKeys[key] == true else string.sub(key, 1, 7) == "engine."
            if not reference[key] and not engines then
                table.insert(own, key)
            end
        end
        table.sort(own)
        for _, key in own do
            table.insert(report.extra, { locale = name, key = key })
        end
    end
    return report
end

export type EditorProblem = {
    --- The line as the engine printed it: `src/a.luau(3,7): error: ...`.
    line: string,
    error: boolean,
}

export type EditorReport = {
    --- False when the engine found here has no checker in it (a player
    --- build), or is one from before it had this command.
    ran: boolean,
    problems: { EditorProblem },
    errors: number,
    --- What the engine said when it could not check at all.
    failure: string?,
}

--- The scenes of a project, as the engine names them: `scenes/menu.scene.json`.
--- A script is mounted by the scene it belongs to (ADR 0105), so each scene is
--- a tree of its own to check.
local function projectScenes(root: string): { string }
    local scenes: { string } = {}
    local function walk(folder: string, relative: string)
        if not fs.exists(folder) then
            return
        end
        for _, entry in fs.listDirectory(folder) do
            local child = projectlib.joined(folder, entry.name)
            local name = if relative == "" then entry.name else `{relative}/{entry.name}`
            if entry.type == "dir" then
                walk(child, name)
            elseif entry.type == "file" and string.sub(entry.name, -11) == ".scene.json" then
                table.insert(scenes, name)
            end
        end
    end
    walk(projectlib.joined(root, "content", "scenes"), "scenes")
    table.sort(scenes)
    return scenes
end

--- **The project as the editor's script pane checks it** (ADR 0093): the
--- engine's own checker over every script of every scene, by `engine-host
--- --check-scripts`. It is why a project this command calls clean opens clean:
--- the analyser above and the editor's checker are two programs, and they were
--- two answers -- a game of a hundred files that passed here was 193 errors in
--- the editor.
---
--- A problem in a script two scenes share is said once.
local function editorCheck(engine: string, root: string): EditorReport
    local report: EditorReport = { ran = true, problems = {}, errors = 0, failure = nil }
    local scenes = projectScenes(root)
    local runs: { { string } } = {}
    if #scenes == 0 then
        table.insert(runs, { engine, root, "--check-scripts" })
    end
    for _, scene in scenes do
        table.insert(runs, { engine, root, "--check-scripts", `--scene={scene}` })
    end
    local seen: { [string]: boolean } = {}
    for _, command in runs do
        local result = process.run(command)
        local output = result.stdout .. "\n" .. result.stderr
        if string.find(output, "engine.cli.err.scripts_no_checker", 1, true) ~= nil then
            report.ran = false
            return report
        end
        local checked = string.find(output, "engine.cli.err.scripts_checked", 1, true) ~= nil
            or string.find(output, "engine.cli.info.scripts_checked", 1, true) ~= nil
            or result.ok
        local found = 0
        for line in string.gmatch(output, "[^\r\n]+") do
            local severity = string.match(line, "^.+%(%d+,%d+%): (%a+): ")
            if severity == "error" or severity == "warning" then
                found += 1
                if not seen[line] then
                    seen[line] = true
                    table.insert(report.problems, { line = line, error = severity == "error" })
                    if severity == "error" then
                        report.errors += 1
                    end
                end
            end
        end
        -- It failed and named no script: the project did not load, or the
        -- engine is one that does not know the command. Its own last words.
        if not result.ok and found == 0 then
            if not checked then
                local last = ""
                for line in string.gmatch(output, "[^\r\n]+") do
                    last = line
                end
                if string.find(last, "check-scripts", 1, true) ~= nil then
                    report.ran = false
                    return report
                end
                report.failure = last
            else
                report.failure = string.match(output, "[^\r\n]*scripts_[^\r\n]*") or ""
            end
            return report
        end
    end
    table.sort(report.problems, function(a: EditorProblem, b: EditorProblem): boolean
        return a.line < b.line
    end)
    return report
end

local function run(options: Options): number
    local files = collectLuau(options.path, options.ignore)
    if #files == 0 then
        -- Nothing to check is not the same as nothing wrong. A check that
        -- passes over an empty file list is the shape of gate this milestone
        -- keeps refusing to build (M2 Finding 19).
        print(tr("cli.check.err.analyze"))
        return 1
    end
    print(tr("cli.check.running", { count = #files }))

    -- **This installation's own analyser, or the one on PATH** (ADR 0152): a
    -- project made a minute ago on a machine with nothing else on it has to
    -- pass its first check.
    local install = projectlib.installRoot()
    local analyser = tools.find("luau-lsp", install)
    if analyser == nil then
        print(tr("cli.check.err.missing_tool", { program = "luau-lsp" }))
        return 2
    end
    -- The formatter is not the installation's to carry (its licence is not
    -- one the engine ships under), so its absence is not this project's
    -- failure: the types are checked, and the formatting is said to be not.
    local formatter = tools.find("stylua", install)

    -- The new solver, which is what R2 says every script is checked by.
    local analyze: { string } = { analyser, "analyze", "--platform=standard", "--flag:LuauSolverV2=true" }
    local scratch = projectlib.joined(options.path, ".engine/check")
    for _, definition in options.definitions do
        -- Named after the file (`@engine`, `@scene`): unnamed, the analyser
        -- announces every one under another platform's name (D212).
        table.insert(analyze, `--definitions=@{definitionName(definition)}={forNewSolver(definition, scratch)}`)
    end
    for _, pattern in options.ignore do
        table.insert(analyze, `--ignore=**/{pattern}/**`)
    end
    for _, file in files do
        table.insert(analyze, file)
    end

    local analyzed = process.run(analyze, { stdio = "inherit" })
    if not analyzed.ok then
        print(tr("cli.check.err.analyze"))
        return 1
    end

    if formatter ~= nil then
        local formatted =
            process.run(styluaCommand({ formatter, "--check" }, options.path, options.ignore), { stdio = "inherit" })
        if not formatted.ok then
            print(tr("cli.check.err.stylua"))
            return 1
        end
    else
        print(tr("cli.check.stylua_skipped"))
    end

    local engine = projectlib.findEngine()
    if engine ~= nil then
        process.run({ engine, "--check-sides", options.path }, { stdio = "inherit" })
    else
        print(tr("cli.check.sides_skipped"))
    end

    -- And as the editor's script pane checks it, for a project (the engine's
    -- own repository is checked by its gate, and has no scene to mount).
    local warnings = 0
    if fs.exists(projectlib.joined(options.path, "project.toml")) then
        if engine == nil then
            print(tr("cli.check.editor_skipped"))
        else
            local editor = editorCheck(engine, options.path)
            if not editor.ran then
                print(tr("cli.check.editor_no_checker"))
            else
                for _, problem in editor.problems do
                    print(problem.line)
                end
                warnings = #editor.problems - editor.errors
                if editor.failure ~= nil then
                    print(tr("cli.check.err.editor_failed", { message = editor.failure :: string }))
                    return 1
                end
                if editor.errors > 0 then
                    print(tr("cli.check.err.editor", { count = editor.errors }))
                    return 1
                end
            end
        end
    end

    -- The game's catalogs against its default locale's (ADR 0154).
    local catalogs = checkCatalogs(options.path, engineCatalogKeys(engine))
    for _, file in catalogs.unreadable do
        print(tr("cli.check.err.catalog_unreadable", { file = file }))
    end
    for _, entry in catalogs.extra do
        print(tr("cli.check.catalog_extra", { locale = entry.locale, key = entry.key }))
    end
    for _, entry in catalogs.missing do
        print(tr("cli.check.err.catalog_missing", { locale = entry.locale, key = entry.key }))
    end
    if #catalogs.missing > 0 or #catalogs.unreadable > 0 then
        print(tr("cli.check.err.i18n"))
        return 1
    end

    if warnings > 0 then
        print(tr("cli.check.ok_warnings", { count = #files, warnings = warnings }))
    else
        print(tr("cli.check.ok", { count = #files }))
    end
    return 0
end

return {
    run = run,
    collectLuau = collectLuau,
    styluaCommand = styluaCommand,
    definitionName = definitionName,
    checkCatalogs = checkCatalogs,
    projectScenes = projectScenes,
    editorCheck = editorCheck,
}
