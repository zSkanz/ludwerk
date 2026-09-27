--!strict
--[[
	`ludwerk saves list|clear [path] [--slot=name] [--player]` (ADR 0111).

	A game's saves, from the command line: what the editor's Play and
	`ludwerk dev` saved into the project's `.engine/saves/`, or -- with
	`--player` -- what the game saved into a player's own folder on this
	machine, which is where SDL puts it: `%APPDATA%/<company>/<name>/saves` on
	Windows, `$XDG_DATA_HOME` (or `~/.local/share`) on Linux, `~/Library/
	Application Support` on macOS. `clear` removes every slot, or the one
	`--slot` names, with its backup.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local platform = require("@std/system/platform")
local process = require("@std/process")

local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

export type Options = {
    action: string?,
    path: string,
    slot: string?,
    player: boolean,
}

local function joined(...: string): string
    return path.format(path.join(...))
end

--- The player's folder for this game, as the engine's `preferencePath` asks
--- SDL for it: the company, then the name.
local function playerFolder(project: projectlib.Project): string?
    local section = project.config.project
    local name = if typeof(section) == "table" and typeof(section.name) == "string" then section.name else nil
    local id = if typeof(section) == "table" and typeof(section.id) == "string" then section.id else nil
    local company = if typeof(section) == "table" and typeof(section.company) == "string" then section.company else nil
    local game = name or id
    if game == nil then
        return nil
    end
    local owner = company or game
    if platform.win32 then
        local appdata = process.env.APPDATA
        return if appdata ~= nil then joined(appdata, owner, game, "saves") else nil
    elseif not platform.linux then
        local home = process.env.HOME
        return if home ~= nil then joined(home, "Library", "Application Support", owner, game, "saves") else nil
    end
    local data = process.env.XDG_DATA_HOME
    if data == nil or data == "" then
        local home = process.env.HOME
        if home == nil then
            return nil
        end
        data = joined(home, ".local", "share")
    end
    return joined(data :: string, owner, game, "saves")
end

--- Every slot under `folder`, and under the one level of folders a match's
--- windows save into, as `name` or `window/name`.
local function slotsIn(folder: string, prefix: string, out: { [string]: { string } }, depth: number)
    if not fs.exists(folder) then
        return
    end
    for _, entry in fs.listDirectory(folder) do
        local file = joined(folder, entry.name)
        if entry.type == "dir" and depth == 0 then
            slotsIn(file, `{entry.name}/`, out, 1)
        elseif entry.type == "file" then
            local stem, suffix = string.match(entry.name, "^(.+)%.(%a+)$")
            if stem ~= nil and (suffix == "save" or suffix == "bak") then
                local key = prefix .. stem
                out[key] = out[key] or {}
                table.insert(out[key], file)
            end
        end
    end
end

local function run(options: Options): number
    local project = projectlib.load(options.path)
    local folder = if options.player then playerFolder(project) else joined(project.root, ".engine", "saves")
    if folder == nil then
        print(tr("cli.saves.no_folder"))
        return 1
    end

    local slots: { [string]: { string } } = {}
    slotsIn(folder, "", slots, 0)
    local names: { string } = {}
    for name in slots do
        if options.slot == nil or name == options.slot or string.match(name, "/(.+)$") == options.slot then
            table.insert(names, name)
        end
    end
    table.sort(names)

    local action = options.action or "list"
    if action == "list" then
        print(tr("cli.saves.folder", { folder = folder }))
        if #names == 0 then
            print(tr("cli.saves.none"))
        end
        for _, name in names do
            print(tr("cli.saves.slot", { name = name }))
        end
        return 0
    elseif action == "clear" then
        for _, name in names do
            for _, file in slots[name] do
                fs.remove(file)
            end
        end
        print(tr("cli.saves.cleared", { count = #names, folder = folder }))
        return 0
    end
    print(tr("cli.saves.unknown_action", { action = action }))
    return 2
end

return {
    run = run,
}
