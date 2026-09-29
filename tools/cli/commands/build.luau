--!strict
--[[
	`ludwerk build` — a game, exported for one target (roadmap M8, ADR 0104).

	**The product is a folder**, and on the desktop an archive of it beside it
	(ADR 0045): not an installer, not a self-extracting executable.

		dist/windows/  MyGame.exe, content/, game/          + MyGame-1.2.0-windows.zip
		dist/linux/    mygame, content/, game/,
		               mygame.desktop, icon.png             + MyGame-1.2.0-linux.tar.gz
		dist/android/  MyGame-1.2.0.apk

	`engine-host` given no script mounts `game/` beside itself. That is the whole
	mechanism -- a convention rather than a configuration file, because a
	configuration file is a second thing that can go missing.

	**Every target from any desktop that has its player** (ADR 0104 §6): a
	Windows or Linux export is a copy, a rename, a stamp and an archive, and an
	Android one is the prebuilt `libmain.so` and Gradle. The steps are the fixed
	list in `export/progress.luau`, which the editor's Export window draws.

	**A dedicated game is two packages** (ADR 0105): `windows`/`linux`/`android`
	are its clients, `windows-server`/`linux-server` its server, and neither
	carries the other's code (`export/game.luau`).
]]

local fs = require("@std/fs")
local json = require("@std/json")
local path = require("@std/path")
local process = require("@std/process")

local android = require("../export/android")
local buildAssets = require("./build-assets")
local catalog = require("../catalog")
local desktop = require("../export/desktop")
local game = require("../export/game")
local output = require("../output")
local progress = require("../export/progress")
local projectlib = require("../project")
local targets = require("../export/targets")
local versionCode = require("../export/version_code")

local tr = catalog.tr

export type Options = {
    path: string,
    -- Where the folder goes. Defaults to `dist/<target>` under the project.
    output: string?,
    target: string,
    -- Clear an output directory this command did not write. Without it, an
    -- existing directory with no build marker in it is refused: `ludwerk build`
    -- deletes what it is pointed at, and being pointed at the wrong thing must
    -- not be silent.
    force: boolean,
    -- Package a host that is not the `player` profile: the debug overlay, the
    -- inspector and a Luau REPL travel with it (D057).
    --
    -- **It exists so that refusing can be the default.** Somebody genuinely
    -- wants a debuggable package now and then, and a rule with no door in it
    -- gets worked around by setting `ENG_HOST`, which is worse because
    -- nothing then says what happened. This says it, every time.
    devHost: boolean?,
    -- One JSON line per step on stdout, for the editor (ADR 0104 §5).
    progress: boolean?,
    -- `[export.android] version_code` plus one, written into `project.toml`
    -- before the export reads it: a store refuses an update whose code is not
    -- higher than the last.
    bumpVersionCode: boolean?,
}

-- What `--target` defaults to: v1's only one, under its current name.
local DefaultTarget = "windows"

-- Written into the output so a rebuild may clear it. Its presence is the whole
-- of the safety check; its contents are for a human who finds it.
local MarkerName = ".engine-build"
local MarkerText = "Written by `ludwerk build`. This directory is cleared on every rebuild.\n"

local MultiplayerModes: { [string]: boolean } = { none = true, host = true, dedicated = true }

local function removeTree(target: string)
    game.removeTree(target)
end

local function stringField(section: any, key: string): string?
    if typeof(section) == "table" and typeof(section[key]) == "string" and section[key] ~= "" then
        return section[key]
    end
    return nil
end

--- What the game is called and who made it (ADR 0104 §1). The file name is
--- sanitised, because it becomes a path: `My Game/2` would otherwise make one.
---
--- **Every field is held to what it is written into** (audit T3, T7): the
--- version names the archive and the APK, so it is a file name's characters
--- too -- `../../x` put them outside `dist/` -- and no field may carry a control
--- character, which in a Linux `.desktop` file is a line of its own.
local function withoutControls(text: string?): string?
    if text == nil then
        return nil
    end
    local cleaned = string.gsub(text, "%c", "")
    return if cleaned == "" then nil else cleaned
end

local function identityOf(project: projectlib.Project): desktop.Identity
    local section = project.config.project
    local displayName = withoutControls(stringField(section, "name")) or path.basename(project.root) or "game"
    local fileName = string.gsub(displayName, "[^%w_%-%.]", "-")
    local icon = stringField(section, "icon")
    local resolved = if icon ~= nil then projectlib.joined(project.root, icon) else nil
    local version = stringField(section, "version")
    if version ~= nil then
        version = string.gsub(version, "[^%w_%-%.%+]", "-")
        -- Dots alone are a directory's name; a version is never only dots.
        if string.match(version :: string, "^%.+$") ~= nil then
            version = nil
        end
    end
    return {
        displayName = displayName,
        fileName = if fileName == "" or string.match(fileName, "^%.+$") ~= nil then "game" else fileName,
        version = version,
        company = withoutControls(stringField(section, "company")),
        icon = if resolved ~= nil and fs.exists(resolved) then resolved else nil,
    }
end

--- `[export] ship_source`: the game's scripts as source rather than bytecode
--- (ADR 0112). Off unless the author asks, for a game meant to be modded or read.
local function shipsSource(project: projectlib.Project): boolean
    local section = project.config.export
    return typeof(section) == "table" and section.ship_source == true
end

--- `[export] multiplayer`, or `none`.
local function multiplayerMode(project: projectlib.Project): (string?, string?)
    local mode = stringField(project.config.export, "multiplayer") or "none"
    if not MultiplayerModes[mode] then
        return nil, tr("cli.build.err.multiplayer_mode", { mode = mode })
    end
    return mode, nil
end

--- A host this machine can RUN, for the steps that boot the engine rather
--- than package it: the partition. The target's own player when it is this
--- machine's; otherwise this machine's player, then its dev host.
local function runnableHost(target: targets.Target, player: string): string?
    local here = targets.hostPlatform()
    if target.platform == here then
        return player
    end
    for _, candidate in targets.All do
        if candidate.platform == here and not candidate.server then
            local found = targets.findPlayer(candidate)
            if found ~= nil then
                return found
            end
        end
    end
    return projectlib.findEngine()
end

--- The SPIR-V shaders an Android package carries: the Android player's own
--- when it has them (an installation), else a desktop build's.
local function spirvShaders(engineContent: string, fallbackHost: string?): string?
    local own = projectlib.joined(engineContent, "shaders")
    if fs.exists(projectlib.joined(own, "manifest.json")) then
        return own
    end
    if fallbackHost ~= nil then
        local shaders = projectlib.joined(targets.contentOf(fallbackHost), "shaders")
        if fs.exists(projectlib.joined(shaders, "manifest.json")) then
            return shaders
        end
    end
    return nil
end

local function run(options: Options): number
    local reporter = progress.new(options.progress == true)
    local fail = function(message: string): number
        progress.note(reporter, message)
        return 1
    end

    local target = targets.find(options.target)
    if target == nil then
        progress.note(
            reporter,
            tr("cli.build.err.unknown_target", { target = options.target, supported = targets.names() })
        )
        return 2
    end
    local chosen = target :: targets.Target

    local project = projectlib.load(options.path)
    if project.diagnostic ~= nil then
        return fail(project.diagnostic :: string)
    end
    if not projectlib.isProject(project) then
        return fail(tr("cli.err.not_a_project", { path = project.root }))
    end
    if options.bumpVersionCode then
        local configFile = projectlib.joined(project.root, "project.toml")
        local bumped, code = versionCode.bump(fs.readFileToString(configFile))
        fs.writeStringToFile(configFile, bumped)
        progress.note(reporter, tr("cli.build.version_code", { code = tostring(code) }))
        project = projectlib.load(options.path)
    end

    local identity = identityOf(project)
    local content, contentOutside = buildAssets.contentDirectory(project)
    if content == nil then
        return fail(contentOutside :: string)
    end
    local exportRoot = projectlib.joined(project.root, ".engine", "export")

    -- **check**: the project's settings, the player, and the tools the target needs.
    local mode = "none"
    local side: game.Side = "all"
    local player = ""
    local engineContent = ""
    local host: string? = nil
    local androidTools: android.Tools? = nil
    local androidSettings: android.Settings? = nil
    local shaders: string? = nil
    local checked = progress.step(reporter, "check", function(): string?
        local configured, modeError = multiplayerMode(project)
        if configured == nil then
            return modeError
        end
        mode = configured
        if chosen.server and mode ~= "dedicated" then
            return tr("cli.build.err.server_needs_dedicated", { target = chosen.name })
        end
        side = if mode ~= "dedicated" then "all" elseif chosen.server then "server" else "client"

        -- **The `player` host, and not just any host** (D057). What a person
        -- edits with carries the overlay, the inspector and a REPL against its
        -- own VM, and what their players receive must not.
        local found = targets.findPlayer(chosen)
        if found == nil then
            local fallback = if chosen.platform == targets.hostPlatform() then projectlib.findEngine() else nil
            if fallback == nil or not options.devHost then
                return tr("cli.build.err.no_player", {
                    target = chosen.name,
                    variable = chosen.variable,
                    preset = chosen.preset,
                })
            end
            progress.note(reporter, tr("cli.build.warn.dev_host", { path = fallback }))
            found = fallback
        end
        player = found :: string
        engineContent = targets.contentOf(player)
        if not fs.exists(engineContent) then
            return tr("cli.build.err.no_engine_content", { path = engineContent })
        end
        host = runnableHost(chosen, player)

        if chosen.platform == "android" then
            local tools, toolsError = android.findTools()
            if tools == nil then
                return toolsError
            end
            androidTools = tools
            local configuredAndroid, settingsError = android.settings(project)
            if configuredAndroid == nil then
                return settingsError
            end
            androidSettings = configuredAndroid
            shaders = spirvShaders(engineContent, host)
            if shaders == nil then
                return tr("cli.build.err.android_no_shaders")
            end
        end
        return nil
    end)
    if not checked then
        return 1
    end

    -- **pack**: first, because the folder ships the built pack rather than the
    -- source art. A side of a dedicated game builds its own from a staged copy
    -- that leaves the other side's scenes and storage out.
    local sidePack: string? = nil
    local sideManifest: string? = nil
    if fs.exists(content) then
        local packed = progress.step(reporter, "pack", function(): string?
            local assetc = projectlib.findTool("assetc")
            if assetc == nil then
                return tr("cli.assets.err.no_compiler")
            end
            -- **Staged for every side**, a one-sided game's too: the scripts a
            -- scene, a stamp or `global.json` carries are compiled in the
            -- staged copy before it is packed (S0.3), so the pack holds no
            -- script's text -- and the project's own files are never touched.
            local stage = projectlib.joined(exportRoot, side)
            local staged = game.stageContent(content, projectlib.joined(stage, "content"), side)
            if not shipsSource(project) then
                if host == nil then
                    return tr("cli.build.err.no_script_compiler")
                end
                local compiled = process.run({ host :: string, "--compile-content-scripts", staged })
                if not compiled.ok then
                    return compiled.stdout .. compiled.stderr
                end
            end
            local pack = projectlib.joined(stage, "content.lpack")
            local manifest = projectlib.joined(stage, "content.manifest.json")
            local built = process.run({ assetc, "--input", staged, "--output", pack, "--manifest", manifest })
            if not built.ok then
                return built.stdout .. built.stderr .. "\n" .. tr("cli.assets.err.failed")
            end
            sidePack = pack
            sideManifest = manifest
            return nil
        end)
        if not packed then
            return 1
        end
    end

    local output = options.output or projectlib.joined(project.root, "dist", chosen.name)
    if fs.exists(output) then
        if not (fs.exists(projectlib.joined(output, MarkerName)) or options.force) then
            return fail(tr("cli.build.err.output_exists", { path = output }))
        end
        removeTree(output)
    end
    fs.createDirectory(output, { makeParents = true })
    fs.writeStringToFile(projectlib.joined(output, MarkerName), MarkerText)

    local composeGame = function(into: string)
        game.compose({
            project = project,
            game = into,
            side = side,
            content = content,
            sidePack = sidePack,
            sideManifest = sideManifest,
        })
    end

    -- **partition**: the streaming cells, pre-warmed into the PACKAGE'S own
    -- `.engine/` (ADR 0053) by the host's own boot stopped before the first
    -- frame. On the package rather than the project, because the cache holds
    -- the scene as the game mounts it -- scripts and all -- and a dedicated
    -- client's cache made from the whole project carried the server's code
    -- (the sentinel test found it). Not fatal: a game whose scene could not be
    -- partitioned partitions itself on first launch.
    local partitionGame = function(gameDir: string)
        progress.step(reporter, "partition", function(): string?
            if host == nil then
                progress.note(reporter, tr("cli.build.warn.no_partition"))
                return nil
            end
            local partition = process.run({ host :: string, gameDir, "--partition" })
            if not partition.ok then
                progress.note(reporter, partition.stdout .. partition.stderr)
                progress.note(reporter, tr("cli.build.warn.no_partition"))
            end
            return nil
        end)
    end

    -- **compile**: every script in the laid-out game to bytecode, by the
    -- engine's own compiler (ADR 0112) -- before the partition, which boots
    -- the game and so reads what it will ship.
    local compileGame = function(gameDir: string): boolean
        if shipsSource(project) then
            return true
        end
        return progress.step(reporter, "compile", function(): string?
            if host == nil then
                return tr("cli.build.err.no_script_compiler")
            end
            local compiled = process.run({ host :: string, "--compile-scripts", gameDir })
            if not compiled.ok then
                return compiled.stdout .. compiled.stderr
            end
            return nil
        end)
    end

    if chosen.platform == "android" then
        local tools = androidTools :: android.Tools
        local settings = androidSettings :: android.Settings
        local stage = projectlib.joined(exportRoot, "android", "stage")
        local laidOut = progress.step(reporter, "layout", function(): string?
            android.stage({
                library = player,
                engineContent = engineContent,
                shaders = shaders :: string,
                stage = stage,
                composeGame = composeGame,
            })
            return nil
        end)
        if not laidOut then
            return 1
        end
        if not compileGame(projectlib.joined(stage, "assets", "game")) then
            return 1
        end
        partitionGame(projectlib.joined(stage, "assets", "game"))

        local resources: string? = nil
        local iconMade = progress.step(reporter, "icon", function(): string?
            local picture = identity.icon
            if picture == nil or string.lower(string.sub(picture, -4)) ~= ".png" then
                return nil
            end
            local assetc = projectlib.findTool("assetc")
            if assetc == nil then
                return tr("cli.assets.err.no_compiler")
            end
            local iconStage = projectlib.joined(exportRoot, "android", "icon")
            removeTree(iconStage)
            local command = { assetc, "icon", picture, "--out", iconStage, "--targets", "android" }
            if settings.background ~= nil then
                table.insert(command, "--background")
                table.insert(command, settings.background :: string)
            end
            if settings.foreground ~= nil then
                table.insert(command, "--foreground")
                table.insert(command, settings.foreground :: string)
            end
            local made = process.run(command)
            if not made.ok then
                return made.stdout .. made.stderr
            end
            resources = projectlib.joined(iconStage, "android", "res")
            return nil
        end)
        if not iconMade then
            return 1
        end

        local apk = ""
        local assembled = progress.step(reporter, "assemble", function(): string?
            android.writePayload(stage)
            -- A release is signed with the project's key and its passwords from
            -- the environment or the per-user store, never from `project.toml`
            -- (ADR 0104 §3); anything else with this machine's debug key.
            local keystore: string
            local keyAlias: string
            local passwords
            if settings.release then
                keystore = settings.keystore :: string
                keyAlias = settings.keyAlias :: string
                passwords = android.releasePasswords(project.root, keystore)
                if passwords == nil then
                    return tr("cli.build.err.android_no_password", { path = keystore })
                end
            else
                local debugKey, keyError = android.debugKeystore(tools)
                if debugKey == nil then
                    return keyError
                end
                keystore = debugKey :: string
                keyAlias = android.DebugAlias
                passwords = { store = android.DebugPassword, key = android.DebugPassword }
            end
            local built, buildError = android.assemble({
                tools = tools,
                stage = stage,
                settings = settings,
                appName = identity.displayName,
                versionName = identity.version or "1.0.0",
                resources = resources,
                keystore = keystore,
                keyAlias = keyAlias,
                passwords = passwords :: any,
                release = settings.release,
            })
            if built == nil then
                return buildError
            end
            local version = if identity.version ~= nil then `-{identity.version}` else ""
            apk = projectlib.joined(output, `{identity.fileName}{version}.apk`)
            fs.copy(built :: string, apk)
            return nil
        end)
        if not assembled then
            return 1
        end
        if
            not progress.step(reporter, "sign", function(): string?
                return android.verify(tools, apk)
            end)
        then
            -- **Not left beside the good ones** (audit T10): an APK whose
            -- signature did not verify sat in `dist/` under the game's name,
            -- the file a person picks up and installs.
            if apk ~= "" and fs.exists(apk) then
                fs.remove(apk)
            end
            return 1
        end
        progress.note(reporter, tr("cli.build.done_android", { path = apk }))
        progress.result(reporter, { target = chosen.name, folder = output, apk = apk, package = settings.package })
        return 0
    end

    -- The desktop: the player under the game's name, the engine's content beside
    -- it, and the game.
    local executableName = desktop.executableName(project, identity, chosen.platform)
    local executable = projectlib.joined(output, executableName)
    local laidOut = progress.step(reporter, "layout", function(): string?
        fs.copy(player, executable)
        -- Less what only an editor reads, as the phone's copy already was: the
        -- new-project template and the editor's icon theme. Every game used to
        -- ship both, a starter project's scripts included.
        game.copyTree(engineContent, projectlib.joined(output, "content"), function(relative: string): boolean
            return relative == "templates" or relative == "icons"
        end)
        composeGame(projectlib.joined(output, "game"))
        if chosen.platform == "linux" and not chosen.server then
            fs.writeStringToFile(
                projectlib.joined(output, `{executableName}.desktop`),
                desktop.desktopEntry(identity, executableName)
            )
        end
        return nil
    end)
    if not laidOut then
        return 1
    end
    if not compileGame(projectlib.joined(output, "game")) then
        return 1
    end
    partitionGame(projectlib.joined(output, "game"))

    -- A server has no face: no icon, and no launcher entry.
    local ico: string? = nil
    if not chosen.server then
        local iconMade = progress.step(reporter, "icon", function(): string?
            local iconStage = projectlib.joined(exportRoot, chosen.platform, "icon")
            removeTree(iconStage)
            local file, iconError = desktop.makeIcon(identity, chosen.platform, iconStage)
            if iconError ~= nil then
                return iconError
            end
            if file ~= nil and chosen.platform == "linux" then
                fs.copy(file :: string, projectlib.joined(output, "icon.png"))
            end
            ico = if chosen.platform == "windows" then file else nil
            return nil
        end)
        if not iconMade then
            return 1
        end
    end

    -- The identity, into the executable and read back out of it: an icon
    -- nobody can assert is an icon that silently regresses.
    if chosen.platform == "windows" then
        if
            not progress.step(reporter, "stamp", function(): string?
                return desktop.stampWindows(executable, ico, identity)
            end)
        then
            return 1
        end
        if ico ~= nil then
            progress.note(reporter, tr("cli.build.icon", { path = identity.icon or "" }))
        end
    end

    local archiveName = desktop.archiveBase(identity, chosen)
        .. (if chosen.platform == "windows" then ".zip" else ".tar.gz")
    local archive = projectlib.joined(path.dirname(output), archiveName)
    if
        not progress.step(reporter, "archive", function(): string?
            local executables = if chosen.platform == "linux" then { executableName } else {}
            return desktop.writeArchive(output, archive, identity.fileName, executables)
        end)
    then
        return 1
    end

    progress.note(reporter, tr("cli.build.done", { path = output, executable = executableName, archive = archive }))
    progress.result(reporter, { target = chosen.name, folder = output, executable = executable, archive = archive })
    return 0
end

--- `ludwerk build --status`: what each target needs and whether this machine has
--- it, as one JSON line, for the editor's target cards (ADR 0104 §4). Asked of
--- the same searches an export makes, so a card that says "ready" is an
--- export that finds its player.
local function status(): number
    local rows: { any } = {}
    local tools = android.findTools()
    for _, target in targets.All do
        local player = targets.findPlayer(target)
        local ready = player ~= nil and (target.platform ~= "android" or tools ~= nil)
        table.insert(rows, {
            name = target.name,
            server = target.server,
            ready = ready,
            player = player ~= nil,
            tools = target.platform ~= "android" or tools ~= nil,
        })
    end
    local fields: { [string]: any } = { targets = rows }
    if tools ~= nil then
        local adb = projectlib.joined(tools.sdk, "platform-tools", projectlib.executableName("adb"))
        if fs.exists(adb) then
            fields.adb = adb
        end
    end
    output.emit(json.serialize(fields :: any))
    return 0
end

return {
    status = status,
    run = run,
    DefaultTarget = DefaultTarget,
    MarkerName = MarkerName,
    editorOnly = game.editorOnly,
}
