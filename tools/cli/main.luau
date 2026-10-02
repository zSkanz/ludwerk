--!strict
--[[
	`ludwerk` — the CLI (roadmap M3, api-design.md §4).

	Lute scripts run by the pinned `lute`, not a `lute compile`d binary (M3
	brief, Decision 2). `lute compile` is host-only as far as anyone has
	documented, so a compiled CLI would have to be produced per tier before any
	gate could run, and a stale binary would silently gate a fresh tree.
	Packaging is an M8 release task, and this file is one step away from it.

	`scripts/ludwerk.ps1` and `scripts/ludwerk.sh` are the wrappers that make the
	documented command line exist.
]]

local fs = require("@std/fs")
local process = require("@std/process")

local androidTools = require("./commands/android")
local build = require("./commands/build")
local buildAssets = require("./commands/build-assets")
local catalog = require("./catalog")
local check = require("./commands/check")
local dev = require("./commands/dev")
local edit = require("./commands/edit")
local fmt = require("./commands/fmt")
local keystore = require("./commands/keystore")
local migrate = require("./commands/migrate")
local new = require("./commands/new")
local projectlib = require("./project")
local saves = require("./commands/saves")
local setup = require("./commands/setup")
local terrain = require("./commands/terrain")
local test = require("./commands/test")
local version = require("./commands/version")

local tr = catalog.tr

-- Vendored trees and Lute's own typedefs. The second does not typecheck under
-- the pinned luau-lsp for upstream reasons (M0 finding 2), and gating on it
-- would fail for something that is not ours to fix.
-- Folder NAMES, matched whole: `dist` is what `ludwerk build` writes, and a
-- check that walked into it analysed every exported copy of the game (D211).
local DefaultIgnore = { "third_party", ".lute", "luau_packages", ".engine", "dist" }

--- `process.args` is not what its name suggests: it carries the script path as
--- its first entry and keeps the `--` separator that was meant to end Lute's
--- own options (U-60). Both are dropped here, so every command below sees the
--- arguments a user actually typed.
local function userArguments(raw: { string }): { string }
    local arguments = table.clone(raw)
    if arguments[1] ~= nil and string.sub(arguments[1], -5) == ".luau" then
        table.remove(arguments, 1)
    end
    if arguments[1] == "--" then
        table.remove(arguments, 1)
    end
    return arguments
end

type Parsed = {
    command: string,
    positional: { string },
    flags: { [string]: string },
}

--- The options that carry a value, which may be written `--out=file` or
--- `--out file`. **Both, because the usage text shows the second** -- and
--- before this list only the first worked: `keystore new --alias upload --out
--- release.keystore` made a keystore named `true` with a key named `true`, and
--- left the two real names lying unread among the positionals.
---
--- Every other option is a switch, and a word after a switch is a positional:
--- `build --dev-host mygame` builds `mygame`.
local ValueFlags: { [string]: boolean } = {
    alias = true,
    definitions = true,
    high = true,
    junit = true,
    low = true,
    material = true,
    name = true,
    octaves = true,
    out = true,
    output = true,
    port = true,
    progress = true,
    report = true,
    scale = true,
    seed = true,
    size = true,
    slot = true,
    target = true,
    template = true,
    validity = true,
}

local function parse(argv: { string }): Parsed
    local parsed: Parsed = { command = "", positional = {}, flags = {} }

    local index = 1
    while index <= #argv do
        local argument = argv[index]
        index += 1
        if index == 2 then
            parsed.command = argument
            continue
        end
        if string.sub(argument, 1, 2) == "--" then
            local body = string.sub(argument, 3)
            local equals = string.find(body, "=", 1, true)
            if equals ~= nil then
                parsed.flags[string.sub(body, 1, equals - 1)] = string.sub(body, equals + 1)
            elseif ValueFlags[body] and index <= #argv and string.sub(argv[index], 1, 2) ~= "--" then
                parsed.flags[body] = argv[index]
                index += 1
            else
                parsed.flags[body] = "true"
            end
            continue
        end
        table.insert(parsed.positional, argument)
    end

    return parsed
end

local function firstPositional(parsed: Parsed, fallback: string): string
    return parsed.positional[1] or fallback
end

local function rejectUnknownFlags(parsed: Parsed, allowed: { string }): boolean
    local known: { [string]: boolean } = {}
    for _, name in allowed do
        known[name] = true
    end
    for name in parsed.flags do
        if not known[name] then
            -- Refused rather than ignored. A misspelt `--headles` that silently
            -- did nothing is the same class of failure as a gate that passes
            -- while doing nothing.
            print(tr("cli.err.unknown_option", { option = name, command = parsed.command }))
            return false
        end
    end
    return true
end

local function main(): number
    local parsed = parse(userArguments(process.args))

    if parsed.command == "" or parsed.flags.help ~= nil or parsed.command == "--help" then
        print(tr("cli.usage"))
        return if parsed.command == "" then 2 else 0
    end

    -- Both spellings `--help` gets, and for the same reason: `--version` is
    -- about the tool rather than about a command, so it is answered before
    -- dispatch and never reaches `rejectUnknownFlags`. There is deliberately
    -- no bare `version` subcommand, because there is no bare `help` one.
    if parsed.command == "--version" or parsed.flags.version ~= nil then
        return version.run()
    end

    if parsed.command == "dev" then
        if not rejectUnknownFlags(parsed, { "headless", "port", "scene", "scene-data" }) then
            return 2
        end
        return dev.run({
            path = firstPositional(parsed, "."),
            headless = parsed.flags.headless ~= nil,
            port = if parsed.flags.port ~= nil then tonumber(parsed.flags.port) else nil,
            scene = parsed.flags.scene,
            sceneData = parsed.flags["scene-data"],
        })
    end

    if parsed.command == "edit" then
        -- No flags at all: `--headless` would be an editor with no window, and
        -- there is no dev server here for a `--port` to name.
        if not rejectUnknownFlags(parsed, {}) then
            return 2
        end
        -- `nil` rather than "." when nothing was typed, which is the difference
        -- between "open this" and "open something" (ADR 0055): the first
        -- refuses a directory that is not a project, and the second is what
        -- opens the launcher.
        return edit.run({ path = parsed.positional[1] })
    end

    if parsed.command == "test" then
        if not rejectUnknownFlags(parsed, { "junit", "report" }) then
            return 2
        end
        return test.run({
            path = firstPositional(parsed, "tests/conformance"),
            junit = parsed.flags.junit,
            report = parsed.flags.report,
        })
    end

    if parsed.command == "check" then
        if not rejectUnknownFlags(parsed, { "definitions" }) then
            return 2
        end
        local definitions: { string } = {}
        if parsed.flags.definitions ~= nil then
            for entry in string.gmatch(parsed.flags.definitions, "[^,]+") do
                table.insert(definitions, entry)
            end
        else
            -- A project's own: the engine's classes, then its scene's tree,
            -- in that order, because the tree's `declare workspace` replaces
            -- the plain one (ADR 0078).
            local types = projectlib.joined(firstPositional(parsed, "."), ".engine", "types")
            for _, name in { "engine.d.luau", "scene.d.luau" } do
                local file = projectlib.joined(types, name)
                if fs.exists(file) then
                    table.insert(definitions, file)
                end
            end
        end
        return check.run({
            path = firstPositional(parsed, "."),
            definitions = definitions,
            ignore = DefaultIgnore,
        })
    end

    if parsed.command == "build-assets" then
        if not rejectUnknownFlags(parsed, { "output", "verify" }) then
            return 2
        end
        return buildAssets.run({
            path = firstPositional(parsed, "."),
            output = parsed.flags.output,
            verify = parsed.flags.verify ~= nil,
        })
    end

    if parsed.command == "build" then
        if
            not rejectUnknownFlags(
                parsed,
                { "output", "target", "force", "dev-host", "progress", "bump-version-code", "status" }
            )
        then
            return 2
        end
        -- What each target needs and whether this machine has it, for the
        -- editor's Export window.
        if parsed.flags.status ~= nil then
            return build.status()
        end
        -- `--progress=json` is the only format there is; anything else is a
        -- typo rather than a request for a format this does not have.
        local progressFlag = parsed.flags.progress
        if progressFlag ~= nil and progressFlag ~= "json" then
            print(tr("cli.build.err.progress_format", { format = progressFlag }))
            return 2
        end
        return build.run({
            path = firstPositional(parsed, "."),
            output = parsed.flags.output,
            target = parsed.flags.target or build.DefaultTarget,
            force = parsed.flags.force ~= nil,
            devHost = parsed.flags["dev-host"] ~= nil,
            progress = progressFlag == "json",
            bumpVersionCode = parsed.flags["bump-version-code"] ~= nil,
        })
    end

    if parsed.command == "android" then
        if not rejectUnknownFlags(parsed, {}) then
            return 2
        end
        if parsed.positional[1] ~= "install-tools" then
            print(tr("cli.android.err.usage"))
            return 2
        end
        return androidTools.run()
    end

    if parsed.command == "keystore" then
        if not rejectUnknownFlags(parsed, { "alias", "out", "name", "validity", "remember", "project" }) then
            return 2
        end
        if parsed.positional[1] == "remember" then
            return keystore.remember(parsed.flags.out, parsed.flags.project)
        end
        if parsed.positional[1] ~= "new" then
            print(tr("cli.keystore.err.usage"))
            return 2
        end
        return keystore.run({
            alias = parsed.flags.alias,
            out = parsed.flags.out,
            name = parsed.flags.name,
            validity = tonumber(parsed.flags.validity),
            remember = parsed.flags.remember ~= nil,
            project = parsed.flags.project,
        })
    end

    -- **A game's saves** (ADR 0111): the project's test saves, or with
    -- `--player` a player's own on this machine.
    if parsed.command == "saves" then
        if not rejectUnknownFlags(parsed, { "slot", "player" }) then
            return 2
        end
        return saves.run({
            action = parsed.positional[1],
            path = parsed.positional[2] or ".",
            slot = parsed.flags.slot,
            player = parsed.flags.player ~= nil,
        })
    end

    -- **A project's terrain from a heightmap, a function or noise** (ADR
    -- 0149 §2): the engine lays it and saves the scene.
    if parsed.command == "terrain" then
        if not rejectUnknownFlags(parsed, { "size", "low", "high", "material", "seed", "scale", "octaves" }) then
            return 2
        end
        return terrain.run({
            action = parsed.positional[1],
            source = parsed.positional[2],
            path = parsed.positional[3] or ".",
            size = parsed.flags.size,
            low = parsed.flags.low,
            high = parsed.flags.high,
            material = parsed.flags.material,
            seed = parsed.flags.seed,
            scale = parsed.flags.scale,
            octaves = parsed.flags.octaves,
        })
    end

    if parsed.command == "fmt" then
        if not rejectUnknownFlags(parsed, {}) then
            return 2
        end
        return fmt.run({ path = firstPositional(parsed, "."), ignore = DefaultIgnore })
    end

    -- **Regenerates `.engine/types/` for the engine that is pinned** (S5.19).
    -- Two documents named this command before it existed: `settings.json` says
    -- it regenerates the types, and the starter's own test says the editor
    -- resolves `@std` and `@engine` against "the stubs `ludwerk setup` generates".
    -- A project cloned from git has none of them, because `.engine/` is
    -- gitignored and only the machine that ran `new` ever wrote it.
    if parsed.command == "setup" then
        if not rejectUnknownFlags(parsed, {}) then
            return 2
        end
        local root = projectlib.installRoot()
        if root == nil then
            print(tr("cli.new.err.no_install"))
            return 1
        end
        return setup.run({ target = firstPositional(parsed, "."), root = root })
    end

    if parsed.command == "migrate" then
        if not rejectUnknownFlags(parsed, {}) then
            return 2
        end
        return migrate.run({ what = parsed.positional[1], path = parsed.positional[2] or "." })
    end

    if parsed.command == "new" then
        if not rejectUnknownFlags(parsed, { "template" }) then
            return 2
        end
        local name = parsed.positional[1]
        if name == nil then
            print(tr("cli.usage"))
            return 2
        end
        return new.run({
            name = name,
            template = parsed.flags.template or "starter",
            parent = new.defaultParent(),
        })
    end

    print(tr("cli.err.unknown_command", { command = parsed.command }))
    return 2
end

process.exit(main())
