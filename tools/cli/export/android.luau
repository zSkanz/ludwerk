--!strict
--[[
	The Android export (ADR 0104 §6): the prebuilt `libmain.so`, the engine's
	content and the game, staged into the one directory the shipped Gradle
	template packages, then signed.

	**No NDK and no CMake.** The player is prebuilt, and the stage is a copy the
	CLI makes itself -- what `cmake/stage_android_player.cmake` did for
	`scripts/android-player.ps1`, which is why that script is now a wrapper
	around this. Gradle is still what assembles the APK, so the export needs a
	JDK 17, the Android SDK and Gradle, and says which one is missing.

	**Gradle is run through `java` directly**, with the launcher class its own
	`gradle.bat` names, rather than through the batch file: a batch file on
	Windows means `cmd /c`, whose quoting a project path or a game name with a
	space in it breaks.

	**The debug key is Android's own**: `~/.android/debug.keystore`, alias
	`androiddebugkey`, the one the SDK's tools make and every other tool on the
	machine signs a debug build with -- so an APK built today installs over
	yesterday's, whichever program built either, and nothing about it depends
	on what this engine is called (D465). Made there, the standard way, when
	the machine has none.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local platform = require("@std/system/platform")
local process = require("@std/process")

local Brand = require("../brand")
local catalog = require("../catalog")
local game = require("./game")
local json = require("@std/json")
local projectlib = require("../project")

local tr = catalog.tr

-- The debug key's fixed secrets. Not secrets at all: a debug key signs a build
-- that installs anywhere and publishes nowhere, and these are the alias and
-- the password Android's own tooling gives it.
local DebugAlias = "androiddebugkey"
local DebugPassword = "android"

local Abi = "arm64-v8a"
local BuildTools = "35.0.0"

local function home(): string
    return path.format(process.homedir())
end

--- `%LOCALAPPDATA%`, on Windows, when it is set.
local function localAppData(): string?
    local value = process.env.LOCALAPPDATA
    return if platform.win32 and value ~= nil and value ~= "" then value else nil
end

--- The user's folder under the brand's name: the Gradle build's cache, and
--- where toolchains were installed before D465. **Nothing a rename would
--- orphan is put here any more** -- see `toolsFolder`.
local function userFolder(): string
    local appData = localAppData()
    if appData ~= nil then
        return projectlib.joined(appData, Brand.read().Name)
    end
    local data = process.env.XDG_DATA_HOME
    if data ~= nil and data ~= "" then
        return projectlib.joined(data, "Ludwerk")
    end
    return projectlib.joined(home(), ".local", "share", Brand.read().Name)
end

--- **Where `ludwerk android install-tools` installs, whatever the engine is
--- called** (D465): `ENG_TOOLS_ROOT` when it is set, otherwise a folder beside
--- Android's own per-user state, named by the code's neutral name (ADR 0109).
--- The folder under the brand's name was the first home of these gigabytes,
--- and a brand file that changed -- or was missing from a package -- left
--- them behind and downloaded them again.
local function toolsFolder(): string
    local root = process.env.ENG_TOOLS_ROOT
    if root ~= nil and root ~= "" then
        return root
    end
    return projectlib.joined(home(), ".android", "engine")
end

--- Where an install goes: the neutral folder, unless this machine already has
--- one under the brand's name -- which is then kept, and never downloaded twice.
local function installFolder(): string
    local neutral = toolsFolder()
    local branded = userFolder()
    local function holds(folder: string): boolean
        return fs.exists(projectlib.joined(folder, "toolchains")) or fs.exists(projectlib.joined(folder, "android-sdk"))
    end
    return if not holds(neutral) and holds(branded) then branded else neutral
end

--- Where `ludwerk android install-tools` puts the SDK.
local function userSdk(): string
    return projectlib.joined(installFolder(), "android-sdk")
end

--- Where the editor keeps its preferences: SDL's per-user folder for
--- `Ludwerk`/`Ludwerk`, which is what `platform::paths().userDir` is. The release
--- passwords live here, outside every project (ADR 0104 §3).
local function preferencesFolder(): string
    local appData = process.env.APPDATA
    if platform.win32 and appData ~= nil and appData ~= "" then
        return projectlib.joined(appData, Brand.read().Name, Brand.read().Name)
    end
    local data = process.env.XDG_DATA_HOME
    if data ~= nil and data ~= "" then
        return projectlib.joined(data, "Ludwerk", "Ludwerk")
    end
    return projectlib.joined(home(), ".local", "share", "Ludwerk", "Ludwerk")
end

local function passwordStore(): string
    return projectlib.joined(preferencesFolder(), "android-keys.json")
end

type Passwords = { store: string, key: string }

local function readStore(): { [string]: Passwords }
    local file = passwordStore()
    if not fs.exists(file) then
        return {}
    end
    local ok, decoded = pcall(json.deserialize, fs.readFileToString(file))
    if not ok or typeof(decoded) ~= "table" then
        return {}
    end
    local out: { [string]: Passwords } = {}
    for keystore, entry in decoded :: { [any]: any } do
        if typeof(keystore) == "string" and typeof(entry) == "table" then
            local store = entry.store
            local key = entry.key
            if typeof(store) == "string" and typeof(key) == "string" then
                out[keystore] = { store = store, key = key }
            end
        end
    end
    return out
end

--- **Whose passwords these are**: the project that remembered them and the
--- keystore, both absolute (audit T5). Keyed by the keystore alone, any
--- project that named its path -- one downloaded, say -- signed with a key
--- whose password was remembered for another.
local function storeKey(project: string, keystore: string): string
    local function absolute(text: string): string
        local resolved = path.format(path.resolve(text))
        return if platform.win32 then string.lower(resolved) else resolved
    end
    return `{absolute(project)}|{absolute(keystore)}`
end

--- Keeps a keystore's passwords in the per-user store, for `project` alone.
--- The file is the user's own: under their profile on Windows, and readable
--- by them alone elsewhere (audit T6). It is not encrypted -- a key kept
--- beside what it opens would protect nothing -- which is why remembering is
--- a choice the person makes, and the environment is the other way in.
local function rememberPasswords(project: string, keystore: string, store: string, key: string)
    local entries = readStore()
    entries[storeKey(project, keystore)] = { store = store, key = key }
    fs.createDirectory(preferencesFolder(), { makeParents = true })
    local file = passwordStore()
    fs.writeStringToFile(file, json.serialize(entries :: any))
    if not platform.win32 then
        process.run({ "chmod", "600", file })
    end
end

--- A release key's passwords: the environment, then the per-user store's for
--- this project and this keystore.
local function releasePasswords(project: string, keystore: string): Passwords?
    local store = process.env.ENG_ANDROID_KEYSTORE_PASSWORD
    if store ~= nil and store ~= "" then
        local key = process.env.ENG_ANDROID_KEY_PASSWORD
        return { store = store, key = if key ~= nil and key ~= "" then key else store }
    end
    return readStore()[storeKey(project, keystore)]
end

--- Where a build of the engine keeps its toolchains, then where an
--- installation keeps them.
local function toolchainRoots(): { string }
    local roots: { string } = {}
    local buildRoot = process.env.ENG_BUILD_ROOT
    if buildRoot ~= nil and buildRoot ~= "" then
        table.insert(roots, projectlib.joined(buildRoot, "toolchains"))
    end
    table.insert(roots, projectlib.joined(toolsFolder(), "toolchains"))
    table.insert(roots, projectlib.joined(userFolder(), "toolchains"))
    local appData = localAppData()
    if appData ~= nil then
        table.insert(roots, projectlib.joined(appData, "Ludwerk", "build", "toolchains"))
    end
    return roots
end

local function exe(name: string): string
    return if platform.win32 then `{name}.exe` else name
end

--- The first directory under `root`, which is how an unpacked JDK names itself.
local function firstDirectory(root: string): string?
    if not fs.exists(root) then
        return nil
    end
    for _, entry in fs.listDirectory(root) do
        if entry.type == "dir" then
            return projectlib.joined(root, entry.name)
        end
    end
    return nil
end

local function findJdk(): string?
    local home = process.env.JAVA_HOME
    if home ~= nil and home ~= "" and fs.exists(projectlib.joined(home, "bin", exe("java"))) then
        return home
    end
    for _, root in toolchainRoots() do
        local jdk = firstDirectory(projectlib.joined(root, "jdk17"))
        if jdk ~= nil and fs.exists(projectlib.joined(jdk, "bin", exe("java"))) then
            return jdk
        end
    end
    return nil
end

--- A Gradle distribution: the directory holding `lib/`.
local function findGradle(): string?
    for _, root in toolchainRoots() do
        if fs.exists(root) then
            for _, entry in fs.listDirectory(root) do
                if entry.type == "dir" and string.match(entry.name, "^gradle%-[%d%.]+$") then
                    return projectlib.joined(root, entry.name)
                end
            end
        end
    end
    return nil
end

local function findSdk(): string?
    for _, variable in { "ANDROID_SDK_ROOT", "ANDROID_HOME" } do
        local value = process.env[variable]
        if value ~= nil and value ~= "" and fs.exists(value) then
            return value
        end
    end
    local candidates: { string } = {
        projectlib.joined(toolsFolder(), "android-sdk"),
        projectlib.joined(userFolder(), "android-sdk"),
    }
    local appData = localAppData()
    if appData ~= nil then
        table.insert(candidates, projectlib.joined(appData, "Android", "Sdk"))
    end
    table.insert(candidates, projectlib.joined(home(), "Android", "Sdk"))
    for _, candidate in candidates do
        if fs.exists(projectlib.joined(candidate, "platforms")) then
            return candidate
        end
    end
    return nil
end

--- The shipped Gradle project the APK is assembled from.
local function findTemplate(): string?
    local root = projectlib.installRoot()
    if root == nil then
        return nil
    end
    local template = projectlib.joined(root, "platforms", "android", "player")
    return if fs.exists(projectlib.joined(template, "settings.gradle")) then template else nil
end

export type Tools = {
    jdk: string,
    gradle: string,
    sdk: string,
    template: string,
}

--- Every tool the export needs, or the message naming the first one missing.
local function findTools(): (Tools?, string?)
    local jdk = findJdk()
    if jdk == nil then
        return nil, tr("cli.build.err.android_no_jdk")
    end
    local gradle = findGradle()
    if gradle == nil then
        return nil, tr("cli.build.err.android_no_gradle")
    end
    local sdk = findSdk()
    if sdk == nil then
        return nil, tr("cli.build.err.android_no_sdk")
    end
    local template = findTemplate()
    if template == nil then
        return nil, tr("cli.build.err.android_no_template")
    end
    return { jdk = jdk, gradle = gradle, sdk = sdk, template = template }, nil
end

export type Settings = {
    package: string,
    versionCode: number,
    -- The manifest's `screenOrientation`, from the start scene.
    orientation: string,
    background: string?,
    foreground: string?,
    -- Signed with the project's own key rather than this machine's debug one.
    release: boolean,
    keystore: string?,
    keyAlias: string?,
    -- A debug key a team shares, in place of this machine's.
    debugKeystore: string?,
    debugKeyAlias: string?,
    -- An App Bundle beside the APK, which a store requires of a new app:
    -- a release makes one unless the project says not, a debug build when it
    -- says so.
    bundle: boolean,
    -- `internet` and `vibrate`, when the project says: otherwise each is
    -- declared when the project's code uses what needs it (`projectUses`).
    internet: boolean?,
    vibrate: boolean?,
}

--- `Enum.ScreenOrientation`, by name and by number, as the manifest spells it.
local SceneOrientations: { [string | number]: string } = {
    LandscapeLeft = "landscape",
    LandscapeRight = "reverseLandscape",
    LandscapeSensor = "sensorLandscape",
    Portrait = "portrait",
    Sensor = "fullSensor",
    [0] = "landscape",
    [1] = "reverseLandscape",
    [2] = "sensorLandscape",
    [3] = "portrait",
    [4] = "fullSensor",
}

--- The engine's `UIService.ScreenOrientation`, for a scene that never set it.
local DefaultOrientation = "sensorLandscape"

--- **The start scene's `UIService.ScreenOrientation`, as the manifest names
--- it** (the owner, 2026-09-27). The manifest is how the phone holds the game
--- from its first frame, before any scene has loaded, and the scene says how
--- it is held once one has. An export setting of its own beside the scene's
--- was two answers to one question, and when they differed the screen turned
--- the moment the game started -- so there is one, and it is the scene's.
local function startOrientation(project: projectlib.Project): string
    local projectSection = project.config.project
    local scene = if typeof(projectSection) == "table" and typeof(projectSection.scene) == "string"
        then projectSection.scene
        else nil
    if scene == nil then
        return DefaultOrientation
    end
    local file = projectlib.joined(project.root, "content", scene)
    if not fs.exists(file) then
        return DefaultOrientation
    end
    local ok, decoded = pcall(json.deserialize, fs.readFileToString(file))
    if not ok or typeof(decoded) ~= "table" then
        return DefaultOrientation
    end
    local document = decoded :: { [string]: any }
    local storage = document.storage
    local service = if typeof(storage) == "table" then storage.UIService else nil
    local properties = if typeof(service) == "table" then service.properties else nil
    local value = if typeof(properties) == "table" then properties.ScreenOrientation else nil
    if (typeof(value) == "string" or typeof(value) == "number") and SceneOrientations[value] ~= nil then
        return SceneOrientations[value]
    end
    return DefaultOrientation
end

--- `[export.android]`, with its defaults (ADR 0104 §3). The message is set
--- when a value is present and wrong, which is refused rather than guessed.
local function settings(project: projectlib.Project): (Settings?, string?)
    local projectSection = project.config.project
    local id = if typeof(projectSection) == "table" and typeof(projectSection.id) == "string"
        then projectSection.id
        else "dev.ludwerk.game"
    local export = project.config.export
    local section = if typeof(export) == "table" and typeof(export.android) == "table" then export.android else {}
    local package = if typeof(section.package) == "string" then section.package else id
    if not string.match(package, "^[%a][%w_]*%.[%a][%w_]*[%w_%.]*$") then
        return nil, tr("cli.build.err.android_package", { package = package })
    end
    local versionCode = if typeof(section.version_code) == "number" then section.version_code else 1
    if versionCode < 1 or versionCode ~= math.floor(versionCode) then
        return nil, tr("cli.build.err.android_version_code", { code = tostring(versionCode) })
    end
    local orientation = startOrientation(project)
    local background = if typeof(section.icon_background) == "string" then section.icon_background else nil
    local foreground = if typeof(section.icon_foreground) == "string"
        then projectlib.joined(project.root, section.icon_foreground)
        else nil
    local release = section.release == true
    local keystore = if typeof(section.keystore) == "string"
        then path.format(path.resolve(projectlib.joined(project.root, section.keystore)))
        else nil
    local keyAlias = if typeof(section.key_alias) == "string" then section.key_alias else nil
    if release and (keystore == nil or keyAlias == nil) then
        return nil, tr("cli.build.err.android_release_key")
    end
    if release and not fs.exists(keystore :: string) then
        return nil, tr("cli.build.err.android_no_keystore", { path = keystore :: string })
    end
    local debugKeystore: string? = if typeof(section.debug_keystore) == "string"
        then path.format(path.resolve(projectlib.joined(project.root, section.debug_keystore)))
        else nil
    local debugKeyAlias: string? = if typeof(section.debug_key_alias) == "string" then section.debug_key_alias else nil
    for _, name in { "internet", "vibrate" } do
        if section[name] ~= nil and typeof(section[name]) ~= "boolean" then
            return nil, tr("cli.build.err.android_permission_value", { name = name })
        end
    end
    if section.bundle ~= nil and typeof(section.bundle) ~= "boolean" then
        return nil, tr("cli.build.err.android_bundle_value")
    end
    local bundle = if typeof(section.bundle) == "boolean" then section.bundle else release
    return {
        package = package,
        versionCode = versionCode,
        orientation = orientation,
        background = background,
        foreground = foreground,
        release = release,
        keystore = keystore,
        keyAlias = keyAlias,
        debugKeystore = debugKeystore,
        debugKeyAlias = debugKeyAlias,
        bundle = bundle,
        internet = section.internet,
        vibrate = section.vibrate,
    },
        nil
end

export type Uses = {
    -- A script names `NetworkService` or requires `@std/net`, or the project
    -- configures a network.
    network: boolean,
    -- A script names `HapticService`.
    haptics: boolean,
}

--- **What the project's own code reaches for that a phone gates behind a
--- permission.** Read from its words, as other engines read theirs: every
--- script under `src/`, and every scene, stamp and `global.json` that carries
--- one. A name built at run time is not seen -- which `[export.android]
--- internet = true` is for -- and a name in a comment is, which costs a
--- permission nobody is asked for.
local function projectUses(project: projectlib.Project): Uses
    local uses: Uses = { network = false, haptics = false }
    local config = project.config
    if typeof(config.network) == "table" or typeof(config.net) == "table" then
        uses.network = true
    end
    local export = config.export
    if typeof(export) == "table" and export.multiplayer == true then
        uses.network = true
    end
    local function read(file: string)
        local text = fs.readFileToString(file)
        if string.find(text, "NetworkService", 1, true) ~= nil or string.find(text, "@std/net", 1, true) ~= nil then
            uses.network = true
        end
        if string.find(text, "HapticService", 1, true) ~= nil then
            uses.haptics = true
        end
    end
    local function walk(folder: string, wanted: (string) -> boolean)
        if not fs.exists(folder) then
            return
        end
        for _, entry in fs.listDirectory(folder) do
            local child = projectlib.joined(folder, entry.name)
            if entry.type == "dir" then
                walk(child, wanted)
            elseif entry.type == "file" and wanted(entry.name) and not (uses.network and uses.haptics) then
                read(child)
            end
        end
    end
    walk(projectlib.joined(project.root, "src"), function(name: string): boolean
        return string.sub(name, -5) == ".luau"
    end)
    walk(projectlib.joined(project.root, "content"), function(name: string): boolean
        return string.sub(name, -11) == ".scene.json" or string.sub(name, -11) == ".stamp.json" or name == "global.json"
    end)
    return uses
end

--- **The permissions the package declares**, in the manifest's order: only
--- what the game uses, because a store's review asks about each one and a
--- player reads the list. All three are normal permissions -- nobody is ever
--- asked anything -- so declaring one costs a line on a store page and not a
--- prompt.
---
--- `INTERNET` is what Android gates every socket behind (D431), and
--- `ACCESS_NETWORK_STATE` goes with it: joining, hosting and HTTPS.
--- `VIBRATE` is `HapticService` on the phone itself.
local function permissions(configured: Settings, uses: Uses): { string }
    local out: { string } = {}
    local internet = if configured.internet ~= nil then configured.internet :: boolean else uses.network
    local vibrate = if configured.vibrate ~= nil then configured.vibrate :: boolean else uses.haptics
    if internet then
        table.insert(out, "android.permission.INTERNET")
        table.insert(out, "android.permission.ACCESS_NETWORK_STATE")
    end
    if vibrate then
        table.insert(out, "android.permission.VIBRATE")
    end
    return out
end

local PermissionsBegin = "<!-- engine:permissions -->"
local PermissionsEnd = "<!-- /engine:permissions -->"

--- The template's manifest with its permissions replaced by `declared`. The
--- template itself declares all of them between two marks, so a package made
--- with no export in front of it can do everything; an export writes its own
--- copy with what its game uses. Nothing when the marks are not there.
local function manifestWith(template: string, declared: { string }): string?
    local from = string.find(template, PermissionsBegin, 1, true)
    local to = string.find(template, PermissionsEnd, 1, true)
    if from == nil or to == nil or to < from then
        return nil
    end
    local lines: { string } = { PermissionsBegin }
    for _, name in declared do
        table.insert(lines, `    <uses-permission android:name="{name}" />`)
    end
    table.insert(lines, "    " .. PermissionsEnd)
    return string.sub(template, 1, from - 1) .. table.concat(lines, "\n") .. string.sub(template, to + #PermissionsEnd)
end

--- Writes the game's manifest into the stage, where the template's build
--- takes it from in place of its own.
local function writeManifest(tools: Tools, stageDir: string, declared: { string }): string?
    local source = projectlib.joined(tools.template, "app", "src", "main", "AndroidManifest.xml")
    local written = if fs.exists(source) then manifestWith(fs.readFileToString(source), declared) else nil
    if written == nil then
        return tr("cli.build.err.android_manifest", { path = source })
    end
    fs.writeStringToFile(projectlib.joined(stageDir, "AndroidManifest.xml"), written :: string)
    return nil
end

--- Every file under `root`, relative to it, sorted: the payload list.
local function listFiles(root: string, relative: string, out: { string })
    for _, entry in fs.listDirectory(root) do
        local rel = if relative == "" then entry.name else `{relative}/{entry.name}`
        local file = projectlib.joined(root, entry.name)
        if entry.type == "dir" then
            listFiles(file, rel, out)
        elseif entry.type == "file" then
            table.insert(out, rel)
        end
    end
end

--- The engine's content for a phone: the player's own, less what only an
--- editor reads, with the shaders in SPIR-V from `shaders` (Android's GPU API
--- is Vulkan; a cross build has no shader toolchain).
local function copyEngineContent(from: string, to: string, shaders: string)
    game.copyTree(from, to, function(relative: string): boolean
        return relative == "shaders" or relative == "templates" or relative == "icons"
    end)
    game.copyTree(shaders, projectlib.joined(to, "shaders"), function(relative: string): boolean
        return relative == "dxil" or relative == "msl"
    end)
end

export type StageOptions = {
    library: string,
    engineContent: string,
    shaders: string,
    stage: string,
    -- Lays the game out into the directory it is given.
    composeGame: (string) -> (),
}

--- `jniLibs/<abi>/libmain.so`, `assets/content/` and `assets/game/`. The
--- payload list is written after, by `writePayload`, once the game's partition
--- cache is in it too.
local function stage(options: StageOptions)
    game.removeTree(options.stage)
    local jni = projectlib.joined(options.stage, "jniLibs", Abi)
    fs.createDirectory(jni, { makeParents = true })
    fs.copy(options.library, projectlib.joined(jni, "libmain.so"))
    local assets = projectlib.joined(options.stage, "assets")
    copyEngineContent(options.engineContent, projectlib.joined(assets, "content"), options.shaders)
    options.composeGame(projectlib.joined(assets, "game"))
end

--- `assets/payload.txt`: the stamp, then every file, which the host extracts
--- on first launch because an APK's assets cannot be listed from native code.
local function writePayload(stageDir: string)
    local assets = projectlib.joined(stageDir, "assets")
    local listing: { string } = {}
    listFiles(projectlib.joined(assets, "content"), "content", listing)
    listFiles(projectlib.joined(assets, "game"), "game", listing)
    table.sort(listing)
    -- The time of staging, so every new APK is extracted once and an
    -- unchanged install is not.
    local stamp = os.date("!%Y%m%dT%H%M%S")
    fs.writeStringToFile(projectlib.joined(assets, "payload.txt"), `{stamp}\n{table.concat(listing, "\n")}\n`)
end

local function java(tools: Tools): string
    return projectlib.joined(tools.jdk, "bin", exe("java"))
end

export type NewKey = {
    keystore: string,
    alias: string,
    storePassword: string,
    keyPassword: string,
    commonName: string,
    days: number,
}

--- `keytool -genkeypair`, with the passwords handed over in the child's
--- environment (`-storepass:env`), never on its command line.
local function newKeystore(tools: Tools, key: NewKey): string?
    local env = table.clone(process.env)
    env.ENG_KEYTOOL_STORE = key.storePassword
    env.ENG_KEYTOOL_KEY = key.keyPassword
    local commonName = string.gsub(key.commonName, "[,=\\]", " ")
    local made = process.run({
        projectlib.joined(tools.jdk, "bin", exe("keytool")),
        "-genkeypair",
        "-keystore",
        key.keystore,
        "-storetype",
        "PKCS12",
        "-storepass:env",
        "ENG_KEYTOOL_STORE",
        "-alias",
        key.alias,
        "-keypass:env",
        "ENG_KEYTOOL_KEY",
        "-keyalg",
        "RSA",
        "-keysize",
        "2048",
        "-validity",
        tostring(key.days),
        "-dname",
        `CN={commonName}`,
    }, { env = env })
    if not made.ok then
        return made.stdout .. made.stderr .. "\n" .. tr("cli.build.err.android_keystore", { path = key.keystore })
    end
    return nil
end

--- **Where the debug key is**: the project's own (`[export.android]
--- debug_keystore`) when it names one, otherwise Android's -- which no name of
--- this engine is part of, so a rename cannot change what a game is signed
--- with (D465).
local function debugKeystorePath(override: string?): string
    return if override ~= nil then override else projectlib.joined(home(), ".android", "debug.keystore")
end

--- The debug key, and the alias it is under. Android's is made the first time,
--- as the SDK's own tools make it: thirty years, `CN=Android Debug`.
local function debugKeystore(tools: Tools, override: string?, overrideAlias: string?): (string?, string?, string?)
    local keystore = debugKeystorePath(override)
    local alias = if overrideAlias ~= nil then overrideAlias else DebugAlias
    if fs.exists(keystore) then
        return keystore, alias, nil
    end
    -- A key the project names is the team's, and is never made here: one made
    -- on this machine would be a different key under the shared one's name.
    if override ~= nil then
        return nil, nil, tr("cli.build.err.android_no_keystore", { path = keystore })
    end
    fs.createDirectory(projectlib.joined(home(), ".android"), { makeParents = true })
    local failed = newKeystore(tools, {
        keystore = keystore,
        alias = DebugAlias,
        storePassword = DebugPassword,
        keyPassword = DebugPassword,
        commonName = "Android Debug",
        days = 10950,
    })
    if failed ~= nil then
        return nil, nil, failed
    end
    return keystore, alias, nil
end

local function gradleBuildRoot(): string
    local buildRoot = process.env.ENG_BUILD_ROOT
    return if buildRoot ~= nil and buildRoot ~= "" then buildRoot else projectlib.joined(userFolder(), "build")
end

export type AssembleOptions = {
    tools: Tools,
    stage: string,
    settings: Settings,
    appName: string,
    versionName: string,
    -- The generated `res/` with the launcher icons, when there is an icon.
    resources: string?,
    keystore: string,
    keyAlias: string,
    passwords: Passwords,
    release: boolean,
    -- Makes the App Bundle instead of the APK: the pack's module folder, as
    -- `stagePack` laid it.
    packDir: string?,
}

--- The name of the pack a bundle carries the game in, which is also its
--- module's and its folder's in the bundle.
local PackName = "game"

--- A folder under another name, by the system's own rename -- at once, on one
--- volume, however large the game -- or copied and removed where that is
--- refused.
local function moveTree(from: string, to: string)
    local native = path.format(path.resolve(from))
    local target = path.format(path.resolve(to))
    local moved = if platform.win32
        then process.run({ "cmd", "/c", "move", "/Y", native, target })
        else process.run({ "mv", native, target })
    if moved.ok and fs.exists(to) and not fs.exists(from) then
        return
    end
    game.copyTree(from, to)
    game.removeTree(from)
end

--- **Lays the game out as a bundle's asset pack**: the module's build file,
--- and the game MOVED from the app's assets to the pack's. An install-time
--- pack is delivered with the app and read as the app's own assets, so the
--- host finds the game where `payload.txt` says it is, bundle or APK.
---
--- Always, and not past some size: a store limits how large the base of a
--- bundle may be, the game is nearly all of an app's bytes, and one way of
--- making a bundle is one way that is tested. `unstagePack` puts it back.
local function stagePack(stageDir: string): string
    -- **The module's folder is named as the pack**: the build finds a pack's
    -- assets by its name, and a module in a folder of another name is packed
    -- with its manifest and nothing else -- silently.
    local pack = projectlib.joined(stageDir, "pack", PackName)
    game.removeTree(projectlib.joined(stageDir, "pack"))
    local assets = projectlib.joined(pack, "src", "main", "assets")
    fs.createDirectory(assets, { makeParents = true })
    fs.writeStringToFile(
        projectlib.joined(pack, "build.gradle"),
        "plugins {\n    id 'com.android.asset-pack'\n}\n\nassetPack {\n"
            .. `    packName = "{PackName}"\n`
            .. '    dynamicDelivery {\n        deliveryType = "install-time"\n    }\n}\n'
    )
    moveTree(projectlib.joined(stageDir, "assets", "game"), projectlib.joined(assets, "game"))
    return pack
end

--- Puts the game back in the app's assets, where an APK takes it from and
--- where the stage is looked at.
local function unstagePack(stageDir: string)
    local held = projectlib.joined(stageDir, "pack", PackName, "src", "main", "assets", "game")
    if fs.exists(held) then
        moveTree(held, projectlib.joined(stageDir, "assets", "game"))
    end
    game.removeTree(projectlib.joined(stageDir, "pack"))
end

--- Gradle on the shipped template. Returns the APK it wrote, or the bundle.
local function assemble(options: AssembleOptions): (string?, string?)
    local tools = options.tools
    local lib = projectlib.joined(tools.gradle, "lib")
    local launcher: string? = nil
    local agent: string? = nil
    for _, entry in fs.listDirectory(lib) do
        if string.match(entry.name, "^gradle%-gradle%-cli%-main%-.+%.jar$") then
            launcher = projectlib.joined(lib, entry.name)
        end
    end
    local agents = projectlib.joined(lib, "agents")
    if fs.exists(agents) then
        for _, entry in fs.listDirectory(agents) do
            if string.match(entry.name, "^gradle%-instrumentation%-agent%-.+%.jar$") then
                agent = projectlib.joined(agents, entry.name)
            end
        end
    end
    if launcher == nil then
        return nil, tr("cli.build.err.android_no_gradle")
    end

    local buildRoot = gradleBuildRoot()
    local command = { java(tools), "-Xmx64m", "-Xms64m" }
    if agent ~= nil then
        table.insert(command, `-javaagent:{agent}`)
    end
    for _, argument in
        {
            "-Dorg.gradle.appname=gradle",
            "-classpath",
            launcher :: string,
            "org.gradle.launcher.GradleMain",
            "--project-dir",
            options.tools.template,
            "--project-cache-dir",
            projectlib.joined(buildRoot, "android-player-gradle", "cache"),
            "--no-daemon",
            "-q",
            if options.packDir ~= nil
                then (if options.release then "bundleRelease" else "bundleDebug")
                else (if options.release then "assembleRelease" else "assembleDebug"),
            `-Pengine.stageDir={options.stage}`,
            `-Pengine.applicationId={options.settings.package}`,
            `-Pengine.appName={options.appName}`,
            `-Pengine.buildRoot={buildRoot}`,
            `-Pengine.versionCode={math.floor(options.settings.versionCode)}`,
            `-Pengine.versionName={options.versionName}`,
            `-Pengine.orientation={options.settings.orientation}`,
            `-Pengine.keystore={options.keystore}`,
            `-Pengine.keyAlias={options.keyAlias}`,
        }
    do
        table.insert(command, argument)
    end
    if options.resources ~= nil then
        table.insert(command, `-Pengine.resDir={options.resources}`)
    end
    if options.packDir ~= nil then
        table.insert(command, `-Pengine.packDir={options.packDir}`)
    end

    local env = table.clone(process.env)
    env.JAVA_HOME = tools.jdk
    env.ANDROID_SDK_ROOT = tools.sdk
    env.ANDROID_HOME = tools.sdk
    -- The passwords as Gradle project properties through the environment
    -- (`ORG_GRADLE_PROJECT_<name>`), so they are on no command line.
    env["ORG_GRADLE_PROJECT_engine.storePassword"] = options.passwords.store
    env["ORG_GRADLE_PROJECT_engine.keyPassword"] = options.passwords.key
    local built = process.run(command, { env = env })
    -- **Once more, with nothing kept, when it is Gradle's own incremental
    -- packaging that fell over.** The Android plugin keeps what it packaged
    -- last time and packages the difference; when what it kept names a file
    -- that is no longer there it dies in `IncrementalSplitterRunnable` with a
    -- null pointer, fifteen seconds in -- and the same command run again
    -- works, because the failure threw the state away. `--rerun-tasks` is
    -- that second run made on purpose: slower, and not dependent on what the
    -- last one left.
    if not built.ok and string.find(built.stdout .. built.stderr, "IncrementalSplitterRunnable", 1, true) ~= nil then
        local again = table.clone(command)
        table.insert(again, "--rerun-tasks")
        built = process.run(again, { env = env })
    end
    if not built.ok then
        return nil, built.stdout .. built.stderr .. "\n" .. tr("cli.build.err.android_gradle")
    end
    local kind = if options.release then "release" else "debug"
    local made = if options.packDir ~= nil
        then projectlib.joined(buildRoot, "android-player-gradle", "app", "outputs", "bundle", kind, `app-{kind}.aab`)
        else projectlib.joined(buildRoot, "android-player-gradle", "app", "outputs", "apk", kind, `app-{kind}.apk`)
    if not fs.exists(made) then
        return nil, tr("cli.build.err.android_gradle")
    end
    return made, nil
end

--- A bundle's signature, by the JDK's own verifier: a bundle is a signed
--- jar, which `apksigner` does not read.
local function verifyBundle(tools: Tools, bundle: string): string?
    -- In English whatever the machine speaks: an unsigned jar "verifies"
    -- with a zero exit and a sentence, so the sentence is what is read.
    local checked = process.run({
        projectlib.joined(tools.jdk, "bin", exe("jarsigner")),
        "-J-Duser.language=en",
        "-J-Duser.country=US",
        "-verify",
        bundle,
    })
    if not checked.ok or string.find(checked.stdout, "jar verified", 1, true) == nil then
        return checked.stdout .. checked.stderr .. "\n" .. tr("cli.build.err.android_unsigned", { path = bundle })
    end
    return nil
end

--- The signature, checked by the SDK's own verifier rather than trusted.
local function verify(tools: Tools, apk: string): string?
    local jar = projectlib.joined(tools.sdk, "build-tools", BuildTools, "lib", "apksigner.jar")
    if not fs.exists(jar) then
        return tr("cli.build.err.android_no_apksigner", { path = jar })
    end
    local checked = process.run({ java(tools), "-jar", jar, "verify", apk })
    if not checked.ok then
        return checked.stdout .. checked.stderr .. "\n" .. tr("cli.build.err.android_unsigned", { path = apk })
    end
    return nil
end

--- What a page is on the largest-paged phones a store requires an app to
--- run on: 16 KB, since Android 15.
local PageBytes = 16384

--- **The smallest alignment of a library's loadable segments**, read from its
--- ELF program headers, or nothing and why for bytes that are not a library.
---
--- It is the number that decides whether a phone with 16 KB pages can load
--- the library at all: a segment aligned to 4 KB cannot be mapped there, and
--- the app dies in `dlopen` before a line of it runs. Read here rather than
--- by a tool because an export has no NDK -- the player is prebuilt -- and
--- the header is thirty lines.
local function loadAlignment(bytes: string): (number?, string?)
    if #bytes < 64 or string.sub(bytes, 1, 4) ~= "\127ELF" then
        return nil, tr("cli.build.elf.not_elf")
    end
    local data = buffer.fromstring(bytes)
    local wide = buffer.readu8(data, 4) == 2
    if buffer.readu8(data, 5) ~= 1 then
        return nil, tr("cli.build.elf.not_little_endian")
    end
    local function word(offset: number): number
        -- A 64-bit field, as a number: no library's offsets or alignments
        -- reach the 2^53 where a double stops being exact.
        return buffer.readu32(data, offset) + buffer.readu32(data, offset + 4) * 4294967296
    end
    local headers = if wide then word(0x20) else buffer.readu32(data, 0x1C)
    local size = buffer.readu16(data, if wide then 0x36 else 0x2A)
    local count = buffer.readu16(data, if wide then 0x38 else 0x2C)
    if count == 0 or size < (if wide then 56 else 32) or headers + count * size > #bytes then
        return nil, tr("cli.build.elf.no_headers")
    end
    local smallest: number? = nil
    for index = 0, count - 1 do
        local at = headers + index * size
        -- PT_LOAD: a segment the loader maps.
        if buffer.readu32(data, at) == 1 then
            local alignment = if wide then word(at + 0x30) else buffer.readu32(data, at + 0x1C)
            smallest = if smallest == nil then alignment else math.min(smallest :: number, alignment)
        end
    end
    if smallest == nil then
        return nil, tr("cli.build.elf.no_segment")
    end
    return smallest, nil
end

--- **Every native library in the package loads on a phone with 16 KB pages**
--- (a store's requirement for an app that targets Android 15 or later, which
--- this one does). The package is opened and each `lib/<abi>/*.so` read: the
--- engine's own is built for it (`cmake/toolchains/android.cmake`), and this
--- is what says so of the file a store will actually receive -- and of any
--- library a later engine, or a project, adds beside it.
local function checkPages(tools: Tools, package: string): string?
    local scratch = `{package}.pages`
    game.removeTree(scratch)
    fs.createDirectory(scratch, { makeParents = true })
    local listed = process.run({ projectlib.joined(tools.jdk, "bin", exe("jar")), "tf", package })
    if not listed.ok then
        game.removeTree(scratch)
        return listed.stdout .. listed.stderr
    end
    local libraries: { string } = {}
    for line in string.gmatch(listed.stdout, "[^\r\n]+") do
        -- An APK keeps them under `lib/`; a bundle under `<module>/lib/`.
        if string.match(line, "^lib/[^/]+/[^/]+%.so$") or string.match(line, "^[^/]+/lib/[^/]+/[^/]+%.so$") then
            table.insert(libraries, line)
        end
    end
    local problem: string? = nil
    if #libraries == 0 then
        problem = tr("cli.build.err.android_no_library", { path = package })
    else
        local command = { projectlib.joined(tools.jdk, "bin", exe("jar")), "xf", path.format(path.resolve(package)) }
        for _, library in libraries do
            table.insert(command, library)
        end
        local extracted = process.run(command, { cwd = scratch })
        if not extracted.ok then
            problem = extracted.stdout .. extracted.stderr
        end
        for _, library in libraries do
            if problem ~= nil then
                break
            end
            local alignment, why = loadAlignment(fs.readFileToString(projectlib.joined(scratch, library)))
            if alignment == nil then
                problem = tr("cli.build.err.android_library_unread", { library = library, why = why :: string })
            elseif alignment < PageBytes then
                problem = tr("cli.build.err.android_pages", { library = library, found = tostring(alignment) })
            end
        end
    end
    game.removeTree(scratch)
    return problem
end

return {
    PageBytes = PageBytes,
    projectUses = projectUses,
    permissions = permissions,
    manifestWith = manifestWith,
    writeManifest = writeManifest,
    loadAlignment = loadAlignment,
    checkPages = checkPages,
    userFolder = userFolder,
    toolsFolder = toolsFolder,
    installFolder = installFolder,
    userSdk = userSdk,
    debugKeystorePath = debugKeystorePath,
    findTools = findTools,
    settings = settings,
    stage = stage,
    writePayload = writePayload,
    debugKeystore = debugKeystore,
    newKeystore = newKeystore,
    rememberPasswords = rememberPasswords,
    releasePasswords = releasePasswords,
    DebugAlias = DebugAlias,
    DebugPassword = DebugPassword,
    assemble = assemble,
    verify = verify,
    PackName = PackName,
    stagePack = stagePack,
    unstagePack = unstagePack,
    verifyBundle = verifyBundle,
}
