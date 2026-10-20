--!strict
--[[
	What a change can affect (ADR 0249): which of the gate's stages to run,
	and which of CTest's tests.

	    lute tools/repo/affected.luau <listing.json> [--base <rev>] [--out <plan.json>]

	`<listing.json>` is CTest's own listing of a configured tree
	(`ctest --show-only=json-v1`). The change is everything between the
	merge base with `--base` (`origin/dev` unless said) and the working tree,
	new files included.

	A file reaches tests three ways:

	  - **its module**: a file under `engine/<module>/` reaches that module's
	    unit tests and those of every module that includes it, from the real
	    `#include "engine/<module>/..."` lines `checklayers.luau` reads;
	  - **its area**: the module, or the file itself in `engine/app/`, names
	    areas -- `render`, `physics`, `net` -- and an area names the tests
	    that run the host on it;
	  - **its path**: a test whose command names the file, or a folder it is
	    in, runs.

	A file no rule maps, the build's own files and the bottom of the engine
	(`core`, `jobs`, `platform`) ask for everything. So does a test this file
	does not know, beside whatever else ran: it is listed, and it runs.

	The plan is printed, and with `--out` written as JSON for
	`scripts/localgate.ps1 -Affected`.
]]

local fs = require("@std/fs")
local json = require("@std/json")
local path = require("@std/path")
local process = require("@std/process")

-- **The tests every change runs**: the host starts, a scene is drawn, a
-- capture is taken, and two runs of one world agree.
local Smoke = { "host_boot", "host_headless_frames", "example_boot_06-scene", "capture_gate", "determinism" }

-- What a change to a module also reaches, beyond its own tests and those of
-- the modules that include it. `everything` is the full suite.
local ModuleAreas: { [string]: { string } } = {
    core = { "everything" },
    jobs = { "everything" },
    platform = { "everything" },
    rhi = { "render" },
    render = { "render" },
    physics = { "physics", "determinism", "netcode" },
    scene = { "scene", "determinism", "netcode" },
    net = { "net", "netcode" },
    replication = { "net", "netcode", "determinism" },
    asset = { "asset" },
    assetc = { "asset" },
    audio = { "audio" },
    input = { "input" },
    nav = { "nav" },
    ui = { "ui" },
    script = { "script" },
    app = { "boot" },
}

-- **A file of `engine/app/` by its name**: the host is one module and a
-- dozen subjects. A file not named here is the host's frame and boot.
local AppFiles: { { pattern: string, areas: { string } } } = {
    { pattern = "^data_", areas = { "data" } },
    { pattern = "headset", areas = { "xr" } },
    { pattern = "^editor", areas = { "editor" } },
    { pattern = "^inspector", areas = { "editor" } },
    { pattern = "^graph_", areas = { "editor" } },
    { pattern = "^node_canvas", areas = { "editor" } },
    { pattern = "^script_editor", areas = { "editor" } },
    { pattern = "^script_complete", areas = { "editor" } },
    { pattern = "^script_syntax", areas = { "editor" } },
    { pattern = "^script_document", areas = { "editor" } },
    { pattern = "^command_palette", areas = { "editor" } },
    { pattern = "^content_tree", areas = { "editor" } },
    { pattern = "^class_favorites", areas = { "editor" } },
    { pattern = "^world_panels", areas = { "editor" } },
    { pattern = "^timeline", areas = { "editor" } },
    { pattern = "^thumbnails", areas = { "editor" } },
    { pattern = "^preview_renderer", areas = { "editor" } },
    { pattern = "_overlay", areas = { "editor" } },
    { pattern = "^reference_grid", areas = { "editor" } },
    { pattern = "^picking", areas = { "editor" } },
    { pattern = "^icons", areas = { "editor" } },
    { pattern = "^launcher", areas = { "editor" } },
    { pattern = "^tip", areas = { "editor" } },
    { pattern = "^profile_history", areas = { "editor" } },
    { pattern = "^script_check", areas = { "script" } },
    { pattern = "^network_session", areas = { "net", "netcode" } },
    { pattern = "^managed_world", areas = { "net", "data" } },
    { pattern = "^manager_", areas = { "net", "data" } },
    { pattern = "^world_manager", areas = { "net", "data" } },
    { pattern = "^match_", areas = { "net" } },
    { pattern = "^admin_", areas = { "net" } },
    { pattern = "^server_console", areas = { "net" } },
    { pattern = "^servers_config", areas = { "net" } },
    { pattern = "^health_server", areas = { "net" } },
    { pattern = "^bots", areas = { "net", "netcode" } },
    { pattern = "^travel_pass", areas = { "net" } },
    { pattern = "^terrain_", areas = { "terrain" } },
    { pattern = "^field_streamer", areas = { "terrain", "asset" } },
    { pattern = "^streaming_host", areas = { "terrain", "asset" } },
    { pattern = "^partition_cache", areas = { "asset" } },
    { pattern = "^ui_", areas = { "ui" } },
    { pattern = "^world_ui", areas = { "ui" } },
    { pattern = "^language_", areas = { "language" } },
    { pattern = "^mouths", areas = { "language" } },
}

-- **What an `@engine/` module reaches** beyond the script area: the tests
-- of the subject it is a module for.
local RuntimeAreas: { [string]: { string } } = {
    xrcomfort = { "xr" },
    xrgrab = { "xr", "netcode" },
    ragdoll = { "physics" },
    camera = { "boot" },
    views = { "boot" },
    splitscreen = { "boot" },
    npclod = { "boot" },
    chat = { "net" },
    guild = { "net" },
}

-- **An area's tests, by name** (Lua patterns). `render` is not here: it is
-- every test that starts a host which draws (below).
local AreaTests: { [string]: { string } } = {
    physics = {
        "^physics",
        "ragdoll",
        "vehicle",
        "breaking_wall",
        "cloth",
        "ceiling_joint",
        "thin_wall",
        "secondary_motion",
        "character",
    },
    determinism = { "^determinism", "^replay", "^clock_differential", "^two_first_opens" },
    netcode = { "^netcode", "^example_multiplayer", "^replica_seam" },
    scene = { "^scene", "^example_boot_24" },
    net = { "^net$", "^app_network", "^serve_", "^data_" },
    data = { "^data_" },
    xr = { "^xr_", "_xr_" },
    asset = { "^asset", "streaming", "^app_streaming", "^render_terrain", "^texture_quality" },
    terrain = { "^terrain", "^render_terrain", "^block_faces", "^chunk_grid", "^foliage", "^example_boot_29" },
    audio = { "^audio", "dubb" },
    language = { "dubb", "^example_cinematic_plays" },
    input = { "^input" },
    nav = { "^nav", "navigation" },
    ui = { "^ui", "^options_screen", "^sprite_color", "^example_boot_25", "^example_boot_3[234]" },
    script = { "^script", "^conformance", "^example_" },
    editor = { "^editor", "^overlay_shots" },
    boot = { "^host_", "^example_boot_" },
}

-- **Stages that build or check one kind of thing**, and the files that
-- reach them beyond the rules below.
local Stages = { "docs", "luau", "format", "windows", "linux", "android", "shipping", "perf", "lavapipe" }

-- **The windows stage's steps after its tests**, which drive the CLI: the
-- conformance specs through `ludwerk test`, the hot-reload round trip, and
-- the packaging chain.
local Steps = { "conformance", "hotreload", "packaging" }

-- The areas the tests that measure time measure.
local PerfAreas = {
    render = true,
    physics = true,
    net = true,
    netcode = true,
    scene = true,
    asset = true,
    terrain = true,
    boot = true,
}

type Plan = {
    full: boolean,
    why: { string },
    areas: { [string]: boolean },
    modules: { [string]: boolean },
    paths: { string },
    -- Tests a changed registration names (`add_test`, `set_tests_properties`).
    registered: { [string]: boolean },
    stages: { [string]: { run: boolean, why: string } },
    steps: { [string]: { run: boolean, why: string } },
}

local function fail(message: string): never
    print(`affected: ERROR: {message}`)
    return process.exit(2)
end

local function run(argv: { string }): string
    local result = process.run(argv)
    if not result.ok then
        fail(`{table.concat(argv, " ")} failed: {result.stderr}`)
    end
    return result.stdout
end

local function lines(text: string): { string }
    local out: { string } = {}
    for line in string.gmatch(text, "[^\r\n]+") do
        table.insert(out, line)
    end
    return out
end

local function startsWith(text: string, prefix: string): boolean
    return string.sub(text, 1, #prefix) == prefix
end

local function endsWith(text: string, suffix: string): boolean
    return #text >= #suffix and string.sub(text, -#suffix) == suffix
end

-- The modules each module includes, from the source itself.
local function includeGraph(): { [string]: { [string]: boolean } }
    local graph: { [string]: { [string]: boolean } } = {}
    for _, entry in fs.listDirectory("engine") do
        if entry.type ~= "dir" then
            continue
        end
        local module = entry.name
        local uses: { [string]: boolean } = {}
        local pending = { path.format(path.join("engine", module)) }
        while #pending > 0 do
            local dir = table.remove(pending) :: string
            for _, child in fs.listDirectory(dir) do
                local childPath = path.format(path.join(dir, child.name))
                if child.type == "dir" then
                    table.insert(pending, childPath)
                elseif endsWith(childPath, ".cpp") or endsWith(childPath, ".h") then
                    for included in string.gmatch(fs.readFileToString(childPath), '#include%s*[<"]engine/([%w_]+)/') do
                        if included ~= module then
                            uses[included] = true
                        end
                    end
                end
            end
        end
        graph[module] = uses
    end
    return graph
end

-- Every module that includes `module`, at any distance, and `module`.
local function dependents(graph: { [string]: { [string]: boolean } }, module: string): { [string]: boolean }
    local reached: { [string]: boolean } = { [module] = true }
    local changed = true
    while changed do
        changed = false
        for name, uses in graph do
            if reached[name] then
                continue
            end
            for used in uses do
                if reached[used] then
                    reached[name] = true
                    changed = true
                    break
                end
            end
        end
    end
    return reached
end

-- The files this change touches, as paths from the repository's root.
local function changedFiles(mergeBase: string): { string }
    local seen: { [string]: boolean } = {}
    local files: { string } = {}
    local function add(file: string)
        if file ~= "" and not seen[file] then
            seen[file] = true
            table.insert(files, file)
        end
    end
    for _, file in lines(run({ "git", "diff", "--name-only", mergeBase })) do
        add(file)
    end
    for _, file in lines(run({ "git", "ls-files", "--others", "--exclude-standard" })) do
        add(file)
    end
    table.sort(files)
    return files
end

local function isCode(file: string): boolean
    return endsWith(file, ".cpp")
        or endsWith(file, ".h")
        or endsWith(file, ".hpp")
        or endsWith(file, ".inl")
        or endsWith(file, ".hlsl")
        or endsWith(file, ".hlsli")
        or endsWith(file, ".mm")
end

-- **Words a test's registration is written in**, and nothing a build is.
local RegistrationWords = {
    "add_test(",
    "set_tests_properties(",
    "COMMAND",
    "PROPERTIES",
    "PASS_REGULAR_EXPRESSION",
    "FAIL_REGULAR_EXPRESSION",
    "SKIP_REGULAR_EXPRESSION",
    "TIMEOUT",
    "LABELS",
    "RESOURCE_LOCK",
    "RESOURCE_GROUPS",
    "RUN_SERIAL",
    "WORKING_DIRECTORY",
    "ENVIRONMENT",
}

-- **The tests a CMake change registers, where that is all it does**: every
-- line it adds or takes away a comment, a blank, or a line of an
-- `add_test` or its properties. Nil where it changes anything else.
local function registeredOnly(file: string, mergeBase: string): { string }?
    local result = process.run({ "git", "diff", "-U0", mergeBase, "--", file })
    if not result.ok then
        return nil
    end
    local names: { string } = {}
    for line in string.gmatch(result.stdout, "[^\r\n]+") do
        local first = string.sub(line, 1, 1)
        if (first == "+" or first == "-") and not startsWith(line, "+++") and not startsWith(line, "---") then
            local body = string.match(string.sub(line, 2), "^%s*(.-)%s*$") or ""
            local test = true
            if body ~= "" and string.sub(body, 1, 1) ~= "#" then
                test = false
                for _, word in RegistrationWords do
                    if string.find(body, word, 1, true) then
                        test = true
                        break
                    end
                end
                -- A command's arguments and the closing of a call.
                test = test
                    or string.sub(body, 1, 2) == "-D"
                    or string.sub(body, 1, 2) == "-P"
                    or string.sub(body, 1, 2) == "--"
                    or body == ")"
                    or string.sub(body, 1, 1) == '"'
            end
            if not test then
                return nil
            end
            local named = string.match(body, "add_test%(NAME%s+([%w_%-%.]+)")
                or string.match(body, "set_tests_properties%(([%w_%-%.]+)")
            if named then
                table.insert(names, named)
            end
        end
    end
    return names
end

-- What one file reaches, written into the plan.
local function mapFile(plan: Plan, file: string, mergeBase: string)
    local function everything(reason: string)
        plan.full = true
        table.insert(plan.why, `{file}: {reason}`)
    end
    local function area(name: string)
        plan.areas[name] = true
    end

    local name = path.basename(file) or file
    if name == "CMakeLists.txt" then
        local names = registeredOnly(file, mergeBase)
        if names ~= nil then
            for _, test in names do
                plan.registered[test] = true
            end
            return
        end
        return everything("the build itself")
    end
    if endsWith(file, ".cmake") and not startsWith(file, "tests/") then
        return everything("the build itself")
    end
    if startsWith(file, "third_party/") or startsWith(file, "cmake/") or name == "CMakePresets.json" then
        return everything("the build or a dependency")
    end
    if
        startsWith(file, "scripts/gates/")
        or file == "scripts/localgate.ps1"
        or startsWith(file, "scripts/docker/")
        or file == "tools/repo/affected.luau"
    then
        return everything("the gate itself")
    end
    if startsWith(file, "engine/platform/src/uwp/") then
        -- Built for the Xbox player alone, and tested by its own executable.
        plan.modules["platform_uwp_io"] = true
        return
    end
    local module = string.match(file, "^engine/([%w_]+)/")
    if module ~= nil then
        local areas = ModuleAreas[module]
        if areas == nil then
            return everything(`engine/{module}/ has no areas in affected.luau`)
        end
        if areas[1] == "everything" then
            return everything(`engine/{module}/ is under every module`)
        end
        plan.modules[module] = true
        if module == "app" then
            local own: { string }? = nil
            for _, rule in AppFiles do
                if string.find(name, rule.pattern) then
                    own = rule.areas
                    break
                end
            end
            for _, named in own or areas do
                area(named)
            end
        else
            for _, named in areas do
                area(named)
            end
        end
        return
    end
    if startsWith(file, "shaders/") then
        plan.modules["render"] = true
        area("render")
        return
    end
    if startsWith(file, "runtime/") then
        area("script")
        local runtimeModule = string.match(file, "^runtime/engine/([%w_]+)/")
        for _, named in (if runtimeModule then RuntimeAreas[runtimeModule] else nil) or {} do
            area(named)
        end
        return
    end
    if file == "i18n/en.json" then
        -- Tests match the engine's own lines, which are this catalogue's.
        area("boot")
        area("net")
        area("data")
        return
    end
    if startsWith(file, "tests/") or startsWith(file, "examples/") then
        table.insert(plan.paths, file)
        return
    end
    if startsWith(file, "docs/") or endsWith(file, ".md") or startsWith(file, "i18n/") then
        return
    end
    if startsWith(file, "tools/") or startsWith(file, ".github/") or startsWith(file, "branding/") then
        return
    end
    if startsWith(file, "platforms/") or startsWith(file, "scripts/") then
        return
    end
    if endsWith(file, ".luau") or endsWith(file, ".toml") or file == ".luaurc" or file == ".styluaignore" then
        return
    end
    return everything("no rule says what it reaches")
end

-- **Which stages a change asks for**, said with why.
local function planStages(plan: Plan, files: { string }, mergeBase: string)
    local function any(test: (string) -> boolean): string?
        for _, file in files do
            if test(file) then
                return file
            end
        end
        return nil
    end
    local function set(stage: string, why: string?, otherwise: string)
        if plan.full then
            plan.stages[stage] = { run = true, why = "the full suite" }
        elseif why ~= nil then
            plan.stages[stage] = { run = true, why = why }
        else
            plan.stages[stage] = { run = false, why = otherwise }
        end
    end

    local code = any(isCode)
    local built = any(function(file)
        return isCode(file)
            or endsWith(file, "CMakeLists.txt")
            or startsWith(file, "tests/")
            or startsWith(file, "examples/")
    end) or any(function(file)
        return startsWith(file, "runtime/") or file == "i18n/en.json"
    end)
    -- What only one platform compiles: its own folders, and code that asks
    -- for it by name -- in the lines changed or the eight round them, not
    -- anywhere in the file: nearly every file of the host has a profile's
    -- `#if` somewhere.
    local function names(file: string, words: { string }): boolean
        if not isCode(file) or not fs.exists(file) then
            return false
        end
        local result = process.run({ "git", "diff", "-U8", mergeBase, "--", file })
        local source = if result.ok and result.stdout ~= "" then result.stdout else fs.readFileToString(file)
        for _, word in words do
            if string.find(source, word, 1, true) then
                return true
            end
        end
        return false
    end
    local android = any(function(file)
        return startsWith(file, "platforms/android/")
            or startsWith(file, "scripts/android")
            or startsWith(file, "tools/cli/export/")
            or startsWith(file, "tests/android/")
            or names(file, { "__ANDROID__", "ENG_PLAYER", "ENG_PROFILE" })
    end)
    local shipping = any(function(file)
        return startsWith(file, "scripts/package")
            or startsWith(file, "scripts/shipping")
            or startsWith(file, "tools/repo/package")
            or names(file, { "ENG_SHIPPING", "ENG_PLAYER", "ENG_PROFILE", "ENG_ENABLE_" })
    end)
    local luau = any(function(file)
        return endsWith(file, ".luau")
            or endsWith(file, ".toml")
            or startsWith(file, "i18n/")
            or startsWith(file, "engine/script/")
            or file == ".luaurc"
            or file == "stylua.toml"
    end)
    local docs = any(function(file)
        return endsWith(file, ".md") or startsWith(file, "docs/") or startsWith(file, "i18n/")
    end) or code
    local perf: string? = nil
    for name in plan.areas do
        if PerfAreas[name] then
            perf = `the {name} area, which the tests that measure time measure`
            break
        end
    end

    set("docs", if docs then `{docs} changed` else nil, "no page, catalogue or code changed")
    set("luau", if luau then `{luau} changed` else nil, "no Luau, project file or script API changed")
    set("format", if code then `{code} changed` else nil, "no C++ or shader changed")
    set("windows", if built then `{built} changed` else nil, "nothing built or tested changed")
    set("linux", if built then `{built} changed` else nil, "nothing built or tested changed")
    set("android", if android then `{android} is built for Android` else nil, "nothing only Android builds changed")
    set(
        "shipping",
        if shipping then `{shipping} is built by a shipped profile` else nil,
        "nothing only a shipped profile builds changed"
    )
    set("perf", perf, "no area the tests that measure time measure changed")
    set("lavapipe", if plan.areas["render"] then "the renderer changed" else nil, "the renderer did not change")

    local function step(name: string, why: string?, otherwise: string)
        if plan.full then
            plan.steps[name] = { run = true, why = "the full suite" }
        elseif why ~= nil then
            plan.steps[name] = { run = true, why = why }
        else
            plan.steps[name] = { run = false, why = otherwise }
        end
    end
    local cli = any(function(file)
        return startsWith(file, "tools/cli/")
    end)
    step(
        "conformance",
        if plan.areas["script"]
            then "the script area changed"
            else cli or any(function(file)
                return startsWith(file, "tests/conformance/")
            end),
        "neither the script API, the specs nor the CLI changed"
    )
    step("hotreload", cli or any(function(file)
        return startsWith(file, "tests/hotreload/") or string.find(file, "reload", 1, true) ~= nil
    end), "neither the CLI nor reloading changed")
    step("packaging", shipping or cli or any(function(file)
        return startsWith(file, "tests/packaging/") or startsWith(file, "branding/")
    end), "nothing a package is made from changed")
end

local function commandOf(test: any): { string }
    local command: { string } = {}
    for _, part in (test.command or {}) :: { any } do
        table.insert(command, tostring(part))
    end
    return command
end

local function labelsOf(test: any): { [string]: boolean }
    local labels: { [string]: boolean } = {}
    for _, property in (test.properties or {}) :: { any } do
        if property.name == "LABELS" then
            for _, label in property.value :: { any } do
                labels[tostring(label)] = true
            end
        end
    end
    return labels
end

-- **Which tests a plan runs**, and why each did.
local function selectTests(plan: Plan, listing: any, root: string): ({ string }, { [string]: string }, { string })
    local chosen: { [string]: string } = {}
    local unknown: { string } = {}
    local function choose(name: string, why: string)
        if chosen[name] == nil then
            chosen[name] = why
        end
    end
    local reachedModules: { [string]: boolean } = {}
    if next(plan.modules) ~= nil then
        local graph = includeGraph()
        for module in plan.modules do
            for name in dependents(graph, module) do
                reachedModules[name] = true
            end
            reachedModules[module] = true
        end
    end
    local all: { string } = {}
    for _, test in listing.tests :: { any } do
        local name = tostring(test.name)
        table.insert(all, name)
        local command = commandOf(test)
        local joined = string.gsub(table.concat(command, " "), "\\", "/")
        local labels = labelsOf(test)
        if labels["gpu-golden"] then
            continue
        end
        for _, smoke in Smoke do
            if smoke == name then
                choose(name, "the smoke set")
            end
        end
        if plan.registered[name] then
            choose(name, "its registration changed")
        end
        -- A module's own executable.
        local exe = string.gsub(command[1] or "", "\\", "/")
        local testModule: string? = string.match(exe, "/engine/[%w_]+/engine_([%w_]+)_tests")
        if testModule ~= nil then
            local own: string = testModule
            if reachedModules[own] then
                choose(name, `the {own} module or one it includes changed`)
            end
            continue
        end
        local host = string.find(joined, "engine-host", 1, true) ~= nil
        -- A test whose command names a changed file or a folder it is in.
        for _, file in plan.paths do
            local absolute = `{root}/{file}`
            for _, part in command do
                local argument = string.gsub(part, "\\", "/")
                local value = string.match(argument, "^%-D[%w_]+=(.*)$") or argument
                -- The repository itself, or all of its tests or examples, is
                -- where a test runs from, not what it runs.
                local whole = value == root or value == `{root}/tests` or value == `{root}/examples`
                if value ~= "" and not whole and (absolute == value or startsWith(absolute, value .. "/")) then
                    choose(name, `it runs {file}`)
                end
            end
            -- The folder of a driver it runs: a driver's project sits beside
            -- it. Not a driver at the top of `tests/`, whose folder is all
            -- of them.
            for _, part in command do
                local value = string.gsub(part, "\\", "/")
                local folder = path.dirname(value) or ""
                if endsWith(value, ".cmake") and folder ~= `{root}/tests` and startsWith(absolute, folder .. "/") then
                    choose(name, `it runs beside {file}`)
                end
            end
        end
        if not host then
            if chosen[name] == nil and not string.find(joined, root, 1, true) then
                table.insert(unknown, name)
            end
            continue
        end
        if plan.areas["render"] and not string.find(joined, "--rhi=null", 1, true) then
            choose(name, "the renderer changed, and it draws")
        end
        for areaName in plan.areas do
            local patterns: { string } = AreaTests[areaName] or {}
            for _, pattern in patterns do
                if string.find(name, pattern) then
                    choose(name, `the {areaName} area changed`)
                end
            end
        end
    end
    -- A test no rule knows runs.
    for _, name in unknown do
        choose(name, "no rule says what reaches it")
    end
    local names: { string } = {}
    for name in chosen do
        table.insert(names, name)
    end
    table.sort(names)
    return names, chosen, all
end

local function main()
    local listingPath: string? = nil
    local base = "origin/dev"
    local out: string? = nil
    local index = 2
    while index <= #process.args do
        local argument = process.args[index]
        if argument == "--base" then
            base = process.args[index + 1]
            index += 2
        elseif argument == "--out" then
            out = process.args[index + 1]
            index += 2
        else
            listingPath = argument
            index += 1
        end
    end
    if listingPath == nil then
        fail(
            "usage: lute tools/repo/affected.luau <ctest --show-only=json-v1 output> [--base <rev>] [--out <plan.json>]"
        )
    end

    local root = string.gsub(string.gsub(run({ "git", "rev-parse", "--show-toplevel" }), "%s+$", ""), "\\", "/")
    local mergeBase = string.gsub(run({ "git", "merge-base", "HEAD", base }), "%s+$", "")
    local files = changedFiles(mergeBase)
    local plan: Plan =
        { full = false, why = {}, areas = {}, modules = {}, paths = {}, registered = {}, stages = {}, steps = {} }
    for _, file in files do
        mapFile(plan, file, mergeBase)
    end
    planStages(plan, files, mergeBase)

    local listing = json.deserialize(fs.readFileToString(listingPath :: string)) :: any
    local names, reasons, all = selectTests(plan, listing, root)

    print(`affected: {#files} file(s) changed since the merge base with {base}`)
    for _, why in plan.why do
        print(`  full suite: {why}`)
    end
    for _, stage in Stages do
        local decided = plan.stages[stage]
        print(`  {if decided.run then "run " else "skip"} {stage}: {decided.why}`)
    end
    for _, step in Steps do
        local decided = plan.steps[step]
        print(`  {if decided.run then "run " else "skip"} windows, {step}: {decided.why}`)
    end
    if plan.full then
        print(`  ran all {#all} tests: the full suite`)
    else
        local areas: { string } = {}
        for name in plan.areas do
            table.insert(areas, name)
        end
        table.sort(areas)
        print(`  ran {#names} of {#all} tests; areas: {if #areas > 0 then table.concat(areas, ", ") else "none"}`)
        for _, name in names do
            print(`    {name}: {reasons[name]}`)
        end
    end

    if out ~= nil then
        local stages: { [string]: any } = {}
        for stage, decided in plan.stages do
            stages[stage] = { run = decided.run, why = decided.why }
        end
        local steps: { [string]: any } = {}
        for step, decided in plan.steps do
            steps[step] = { run = decided.run, why = decided.why }
        end
        local escaped: { string } = {}
        for _, name in names do
            table.insert(escaped, (string.gsub(name, "([%.%+%-%*%?%[%]%^%$%(%)])", "\\%1")))
        end
        local document = {
            full = plan.full,
            why = plan.why,
            tests = names,
            regex = if plan.full then "" else `^({table.concat(escaped, "|")})$`,
            stages = stages,
            steps = steps,
            summary = if plan.full then `ran all {#all} tests: the full suite` else `ran {#names} of {#all} tests`,
        }
        fs.writeStringToFile(out :: string, json.serialize(document :: any))
    end
end

main()
