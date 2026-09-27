--!strict
--[[
	What `ludwerk build --target=<name>` can build, and where the player for each
	one is (ADR 0104 §6).

	**A target is a player, not a machine.** Windows and Linux exports are a
	copy, a rename, a stamp and an archive, so each is built from any desktop
	that has the player for it -- which is why nothing here asks what this
	machine is, except to offer the dev host as a fallback that can only ever
	be this machine's own.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local platform = require("@std/system/platform")
local process = require("@std/process")

local projectlib = require("../project")

export type Target = {
    -- What `--target` names, and the folder under `dist/`.
    name: string,
    -- Which player it packages: "windows", "linux" or "android".
    platform: string,
    -- A dedicated server's package: no icon, no client code (ADR 0105).
    server: boolean,
    -- The player's file name, and where each place keeps it.
    binary: string,
    -- Under `player/` in an installation (ADR 0104 §6).
    installDir: string,
    -- The build preset that produces it, under `ENG_BUILD_ROOT`.
    preset: string,
    -- Where a build tree puts it, relative to the preset directory.
    buildPath: string,
    -- The variable that points at one somewhere else.
    variable: string,
}

local Windows = {
    platform = "windows",
    binary = "engine-host.exe",
    installDir = "windows-x64",
    preset = "win-msvc-player",
    buildPath = "engine/app/engine-host.exe",
    variable = "ENG_PLAYER_HOST_WINDOWS",
}

local Linux = {
    platform = "linux",
    binary = "engine-host",
    installDir = "linux-x64",
    preset = "linux-clang-player",
    buildPath = "engine/app/engine-host",
    variable = "ENG_PLAYER_HOST_LINUX",
}

local Android = {
    platform = "android",
    binary = "libmain.so",
    installDir = "android-arm64",
    preset = "android-arm64-player",
    buildPath = "engine/app/libmain.so",
    variable = "ENG_PLAYER_HOST_ANDROID",
}

local function target(name: string, base: typeof(Windows), server: boolean): Target
    return {
        name = name,
        platform = base.platform,
        server = server,
        binary = base.binary,
        installDir = base.installDir,
        preset = base.preset,
        buildPath = base.buildPath,
        variable = base.variable,
    }
end

--- In the order `ludwerk build --help` and the Export window list them.
local All: { Target } = {
    target("windows", Windows, false),
    target("linux", Linux, false),
    target("android", Android, false),
    target("windows-server", Windows, true),
    target("linux-server", Linux, true),
}

--- The names v1 used, kept working: `win64` was the only target there was.
local Aliases: { [string]: string } = {
    win64 = "windows",
}

local function find(name: string): Target?
    local wanted = Aliases[name] or name
    for _, candidate in All do
        if candidate.name == wanted then
            return candidate
        end
    end
    return nil
end

local function names(): string
    local list: { string } = {}
    for _, candidate in All do
        table.insert(list, candidate.name)
    end
    return table.concat(list, ", ")
end

--- The platform this CLI is running on, as a target platform names it.
local function hostPlatform(): string?
    if platform.win32 then
        return "windows"
    end
    if platform.linux then
        return "linux"
    end
    return nil
end

export type Roots = {
    -- `ENG_PLAYER_HOST_<TARGET>`, or `ENG_PLAYER_HOST` for this machine's
    -- own platform (the one v1 read).
    explicit: string?,
    buildRoot: string?,
    install: string?,
}

--- Every path the player for `target` could be at, in the order tried. Pure,
--- so a test holds the order still.
---
--- **The variable first**, although the ledger listed it last: a variable that
--- lost to whatever happened to be on disk would not be an override, and it is
--- how a CI job points at the player it just built.
local function playerCandidates(chosen: Target, roots: Roots): { string }
    local candidates: { string } = {}
    if roots.explicit ~= nil and roots.explicit ~= "" then
        table.insert(candidates, roots.explicit)
    end
    if roots.install ~= nil and roots.install ~= "" then
        table.insert(candidates, projectlib.joined(roots.install, "player", chosen.installDir, chosen.binary))
        -- v1's installation kept this machine's player directly under
        -- `player/`; stage 4 of ADR 0104 lays one out per target.
        if chosen.platform == hostPlatform() then
            table.insert(candidates, projectlib.joined(roots.install, "player", chosen.binary))
        end
    end
    if roots.buildRoot ~= nil and roots.buildRoot ~= "" then
        table.insert(candidates, projectlib.joined(roots.buildRoot, chosen.preset, chosen.buildPath))
    end
    return candidates
end

local function environmentRoots(chosen: Target): Roots
    local explicit = process.env[chosen.variable]
    if (explicit == nil or explicit == "") and chosen.platform == hostPlatform() then
        explicit = process.env.ENG_PLAYER_HOST
    end
    return {
        explicit = explicit,
        buildRoot = process.env.ENG_BUILD_ROOT,
        install = projectlib.installRoot(),
    }
end

local function findPlayer(chosen: Target): string?
    for _, candidate in playerCandidates(chosen, environmentRoots(chosen)) do
        if fs.exists(candidate) then
            return candidate
        end
    end
    return nil
end

--- The engine's own content beside a player: catalogs, shaders, fonts, @engine.
local function contentOf(player: string): string
    return projectlib.joined(path.dirname(player), "content")
end

return {
    All = All,
    find = find,
    names = names,
    hostPlatform = hostPlatform,
    playerCandidates = playerCandidates,
    findPlayer = findPlayer,
    contentOf = contentOf,
}
