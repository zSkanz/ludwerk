--!strict
--[[
	The Play rule's lint.

	**Every command the editor's shell can ask for says whether a running game
	refuses it.** A command is a field of `EditorCommands`; while the game runs,
	`EditorCommands::refuseAuthoring` takes the authoring ones off the list
	before the loop acts on any, and `EditorCommands::keptInPlay` names the
	rest. The rule had been a check at each panel that remembered to make one,
	and a click during Play selected what was behind the game's own button, a
	brush sculpted the running world and Ctrl+Z put an editing snapshot into
	it -- every one of them a site nobody had thought about.

	So a field in neither list fails the gate, and so does one in both, and a
	name in either list that is not a field any more.

	Run from the repository root:

	    lute tools/repo/playlint.luau
]]

local fs = require("@std/fs")
local process = require("@std/process")

local Header = "engine/app/include/engine/app/editor.h"
local Source = "engine/app/src/editor.cpp"

local function linesOf(file: string): { string }
    local lines: { string } = {}
    for line in string.gmatch(fs.readFileToString(file) .. "\n", "([^\n]*)\n") do
        table.insert(lines, (string.gsub(line, "\r$", "")))
    end
    return lines
end

--- The lines from the one holding `opening` to the first that is `closing`.
local function block(lines: { string }, opening: string, closing: string): { string }?
    local out: { string } = {}
    local inside = false
    for _, line in lines do
        if not inside then
            inside = string.find(line, opening, 1, true) ~= nil
        elseif string.find(line, closing, 1, true) == 1 then
            return out
        else
            table.insert(out, line)
        end
    end
    return nil
end

local problems: { string } = {}

local header = linesOf(Header)
local struct = block(header, "struct EditorCommands", "    void clear() noexcept")
local fields: { string } = {}
local isField: { [string]: boolean } = {}
if struct == nil then
    table.insert(problems, `{Header}: no EditorCommands struct ending at clear() -- this lint stopped reading it`)
else
    for _, line in struct do
        -- A member is declared at the struct's own indent; a comment is not one.
        if string.sub(line, 1, 4) ~= "    " or string.sub(line, 5, 5) == " " or string.sub(line, 5, 6) == "//" then
            continue
        end
        local code = string.gsub(line, "%s*//.*$", "")
        local name = string.match(code, "([%a_][%w_]*)%s*=.*;$") or string.match(code, "([%a_][%w_]*);$")
        if name ~= nil then
            table.insert(fields, name)
            isField[name] = true
        end
    end
end

local source = linesOf(Source)
local refused: { [string]: boolean } = {}
local kept: { [string]: boolean } = {}
local refuseBody = block(source, "bool EditorCommands::refuseAuthoring() noexcept", "}")
local keptBody = block(source, "EditorCommands::keptInPlay() noexcept", "}")
if refuseBody == nil then
    table.insert(problems, `{Source}: EditorCommands::refuseAuthoring is gone -- a running game refuses nothing`)
else
    for _, line in refuseBody do
        local name = string.match(line, "drop%(([%a_][%w_]*),")
        if name ~= nil then
            refused[name] = true
        end
    end
end
if keptBody == nil then
    table.insert(problems, `{Source}: EditorCommands::keptInPlay is gone`)
else
    for _, line in keptBody do
        local name = string.match(line, '^%s*"([%a_][%w_]*)",')
        if name ~= nil then
            kept[name] = true
        end
    end
end

for _, name in fields do
    if refused[name] and kept[name] then
        table.insert(problems, `EditorCommands::{name} is both refused in Play and kept in it`)
    elseif not refused[name] and not kept[name] then
        table.insert(
            problems,
            `EditorCommands::{name} does not say what a running game does with it: `
                .. `drop it in refuseAuthoring (it changes the scene) or name it in keptInPlay ({Source})`
        )
    end
end
for name in refused do
    if not isField[name] then
        table.insert(problems, `refuseAuthoring drops '{name}', which is not a field of EditorCommands`)
    end
end
for name in kept do
    if not isField[name] then
        table.insert(problems, `keptInPlay names '{name}', which is not a field of EditorCommands`)
    end
end

if #problems > 0 then
    for _, problem in problems do
        print(`playlint: ERROR: {problem}`)
    end
    process.exit(1)
end
local refusedCount = 0
for _ in refused do
    refusedCount += 1
end
print(`playlint: ok -- {#fields} editor command(s), {refusedCount} refused while the game runs`)
