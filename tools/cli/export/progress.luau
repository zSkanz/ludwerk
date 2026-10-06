--!strict
--[[
	The steps an export goes through, said as it goes (ADR 0104 §5).

	**One list, and it is fixed.** The editor's Export window draws its
	progress bar from these names, so a step renamed here is a bar that stops
	moving there; `docs/manual/reference/cli.md` documents the same list.

	With `--progress=json` each step is a line of JSON on stdout, for a program
	to read -- `{"step":"pack","state":"done","ms":812}` -- and that is bytes
	for another program, so it goes out through `output.emit`, untranslated.
	Without it, the same steps are sentences for a person, through the catalog.
]]

local json = require("@std/json")
local time = require("@std/time")

local catalog = require("../catalog")
local output = require("../output")

local tr = catalog.tr

--- In the order a build takes them. Not every target takes every step: the
--- icon has no server step, and only Android assembles and signs.
local Steps = {
    "check", -- the project, the player for the target, the tools it needs
    "pack", -- the content pack, per side when the game is dedicated
    "layout", -- the player, the engine's content and the game, into the folder
    "compile", -- the game's scripts to bytecode, unless it ships source (ADR 0112)
    "partition", -- the streaming cells, pre-warmed into the package's `.engine/`
    "seal", -- the scripts, the text and the settings into the pack (ADR 0183)
    "icon", -- one PNG, every size the target needs
    "stamp", -- the icon and the version into the executable
    "archive", -- the `.zip` or the `.tar.gz` beside the folder
    "assemble", -- Gradle, on the shipped template
    "sign", -- the APK's signature, checked
}

-- Each step's name for a person. Functions of literal keys, rather than a key
-- built from the name, so the i18n lint sees every one of them.
local Labels: { [string]: () -> string } = {
    check = function()
        return tr("cli.build.step.check")
    end,
    pack = function()
        return tr("cli.build.step.pack")
    end,
    compile = function()
        return tr("cli.build.step.compile")
    end,
    partition = function()
        return tr("cli.build.step.partition")
    end,
    seal = function()
        return tr("cli.build.step.seal")
    end,
    layout = function()
        return tr("cli.build.step.layout")
    end,
    icon = function()
        return tr("cli.build.step.icon")
    end,
    stamp = function()
        return tr("cli.build.step.stamp")
    end,
    archive = function()
        return tr("cli.build.step.archive")
    end,
    assemble = function()
        return tr("cli.build.step.assemble")
    end,
    sign = function()
        return tr("cli.build.step.sign")
    end,
}

export type Reporter = {
    json: boolean,
}

local function new(asJson: boolean): Reporter
    return { json = asJson }
end

local function emitJson(step: string, state: string, ms: number?, message: string?)
    local line: { [string]: any } = { step = step, state = state }
    if ms ~= nil then
        line.ms = math.floor(ms + 0.5)
    end
    if message ~= nil and message ~= "" then
        line.message = message
    end
    output.emit(json.serialize(line :: any))
end

--- Runs `body` as the step `name`. `body` returns nil on success, or the
--- message that says what went wrong -- already translated, because it is what
--- a person reads under the red step.
local function step(reporter: Reporter, name: string, body: () -> string?): boolean
    assert(Labels[name] ~= nil, `progress: "{name}" is not one of the documented steps`)
    local started = time.now()
    if reporter.json then
        emitJson(name, "start")
    else
        print(tr("cli.build.step.start", { step = Labels[name]() }))
    end
    local ok, failure = pcall(body)
    local message: string? = if ok then failure else tostring(failure)
    local ms = time.since(started) * 1000
    if message == nil then
        if reporter.json then
            emitJson(name, "done", ms)
        end
        return true
    end
    if reporter.json then
        emitJson(name, "fail", ms, message)
    else
        print(message)
    end
    return false
end

--- A line that belongs to no step: the path at the end, a warning.
local function note(reporter: Reporter, message: string)
    if reporter.json then
        output.emit(json.serialize({ note = message } :: any))
    else
        print(message)
    end
end

--- What an export made, for a program: one `{"result": ...}` line in JSON
--- mode, nothing otherwise (the sentence before it said it for a person).
local function result(reporter: Reporter, fields: { [string]: string })
    if reporter.json then
        output.emit(json.serialize({ result = fields } :: any))
    end
end

return {
    result = result,
    Steps = Steps,
    new = new,
    step = step,
    note = note,
}
