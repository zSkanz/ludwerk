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
]]

local fs = require("@std/fs")
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

--- **The definitions, as the new solver can read them** (R2, D192). The
--- generated file types `Instance.new` as an intersection of one overload per
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

    print(tr("cli.check.ok", { count = #files }))
    return 0
end

return {
    run = run,
    collectLuau = collectLuau,
    styluaCommand = styluaCommand,
    definitionName = definitionName,
}
