--!strict
local android = require("../export/android")
local androidTools = require("../commands/android")
local catalog = require("../catalog")
local fs = require("@std/fs")
local game = require("../export/game")
local json = require("@std/json")
local path = require("@std/path")
local process = require("@std/process")
local projectlib = require("../project")
local registry = require("./registry")
local targets = require("../export/targets")
local tr = catalog.tr
local function folder(item: registry.Module): string
    return projectlib.joined(android.toolsFolder(), "modules", item.Id, item.Version or "current")
end
local function enabled(config: any, id: string): boolean
    local values = config.integrations
    return typeof(values) == "table" and typeof(values.enabled) == "table" and table.find(values.enabled, id) ~= nil
end
local function nativeDirectory(item: registry.Module): string
    return projectlib.joined(folder(item), "sdk", item.NativeDirectory or "")
end
local function owned(item: registry.Module): boolean
    local marker = projectlib.joined(folder(item), ".engine-module")
    return fs.exists(marker) and fs.readFileToString(marker) == `{item.Id}\n{item.Version}\n`
end
local function available(item: registry.Module): boolean
    if item.Kind == "builtin" then
        local target = targets.find(item.Targets[1])
        return target ~= nil and targets.findPlayer(target) ~= nil
    end
    if item.Installer == "android" then
        return android.findTools() ~= nil
    end
    if not owned(item) then
        return false
    end
    for _, file in item.RequiredFiles or {} do
        if not fs.exists(projectlib.joined(nativeDirectory(item), file)) then
            return false
        end
    end
    return true
end
local function completeProvider(item: registry.Module, directory: string): boolean
    if item.NativeFiles == nil then
        return false
    end
    for _, file in item.NativeFiles do
        if not fs.exists(projectlib.joined(directory, file)) then
            return false
        end
    end
    return true
end
-- Sources belong to the engine installation/build, never to a downloaded game.
local function providerDirectory(item: registry.Module): string?
    local own = projectlib.joined(folder(item), "runtime")
    if completeProvider(item, own) then
        return own
    end
    for _, root in { projectlib.installRoot() or "", process.env.ENG_BUILD_ROOT or "" } do
        if root == "" then
            continue
        end
        local source = projectlib.joined(root, "modules", item.Id, item.Version or "current", item.Payload or "")
        if completeProvider(item, source) then
            return source
        end
    end
    return nil
end
local function verifyProvider(item: registry.Module, directory: string): boolean
    local manifest = projectlib.joined(directory, "provider.json")
    if not fs.exists(manifest) then
        return false
    end
    local ok, decoded = pcall(json.deserialize, fs.readFileToString(manifest))
    local metadata = if ok then json.asObject(decoded) else nil
    if metadata == nil or metadata.id ~= item.Id or metadata.version ~= item.Version or metadata.abi ~= 1 then
        return false
    end
    local hashes = json.asObject(metadata.files)
    if hashes == nil then
        return false
    end
    for _, file in item.NativeFiles or {} do
        if androidTools.sha256Of(projectlib.joined(directory, file)) ~= hashes[file] then
            return false
        end
    end
    return true
end
local function verify(item: registry.Module): boolean
    if not available(item) then
        local directory = providerDirectory(item)
        return directory ~= nil and verifyProvider(item, directory)
    end
    local installedProvider = projectlib.joined(folder(item), "runtime")
    if fs.exists(installedProvider) and not verifyProvider(item, installedProvider) then
        return false
    end
    if item.Installer ~= "nuget" then
        return true
    end
    local archive = projectlib.joined(folder(item), "sdk.nupkg")
    local inventory = projectlib.joined(folder(item), "inventory.json")
    if not fs.exists(archive) or androidTools.sha256Of(archive) ~= item.Sha256 or not fs.exists(inventory) then
        return false
    end
    local ok, decoded = pcall(json.deserialize, fs.readFileToString(inventory))
    if not ok then
        return false
    end
    local hashes = json.asObject(decoded)
    if hashes == nil then
        return false
    end
    for _, file in item.RequiredFiles or {} do
        if androidTools.sha256Of(projectlib.joined(nativeDirectory(item), file)) ~= hashes[file] then
            return false
        end
    end
    return true
end
local function rows(config: any): { any }
    local out: { any } = {}
    for _, item in registry.All do
        local installed = available(item)
        local settings: { any } = {}
        for _, field in item.Settings or {} do
            local value: any = config
            for _, part in string.split(field.Key, ".") do
                value = if typeof(value) == "table" then value[part] else nil
            end
            table.insert(
                settings,
                { key = field.Key, label = tr(field.Label), value = if typeof(value) == "string" then value else "" }
            )
        end
        table.insert(out, {
            id = item.Id,
            label = tr(item.Label),
            kind = item.Kind,
            integration = item.Integration or "",
            enabled = item.Integration ~= nil and enabled(config, item.Integration),
            installed = installed,
            provider = providerDirectory(item) ~= nil,
            supported = item.Host == nil or item.Host == targets.hostPlatform(),
            version = item.Version or "",
            bytes = item.Bytes or 0,
            settings = settings,
            removable = item.Installer == "nuget" and owned(item),
        })
    end
    return out
end
local function installPackage(item: registry.Module, archive: string, expected: string): number
    if item.NativeFiles == nil or item.Host ~= targets.hostPlatform() then
        return 1
    end
    if
        not fs.exists(archive)
        or #expected ~= 64
        or string.find(expected, "[^%x]") ~= nil
        or androidTools.sha256Of(archive) ~= string.lower(expected)
    then
        print(tr("cli.modules.package_checksum"))
        return 1
    end
    local destination = folder(item)
    local staging = projectlib.joined(destination, "incoming")
    local failed = androidTools.unpack(archive, staging, nil)
    if failed then
        print(failed)
        return 1
    end
    local source = projectlib.joined(staging, "modules", item.Id, item.Version or "current", item.Payload or "")
    local manifest = projectlib.joined(source, "provider.json")
    if not fs.exists(manifest) then
        print(tr("cli.modules.package_invalid"))
        return 1
    end
    local ok, decoded = pcall(json.deserialize, fs.readFileToString(manifest))
    local metadata = if ok then json.asObject(decoded) else nil
    if metadata == nil or metadata.id ~= item.Id or metadata.version ~= item.Version or metadata.abi ~= 1 then
        print(tr("cli.modules.package_invalid"))
        return 1
    end
    local hashes = json.asObject(metadata.files)
    if hashes == nil then
        print(tr("cli.modules.package_invalid"))
        return 1
    end
    for _, file in item.NativeFiles do
        local input = projectlib.joined(source, file)
        if not fs.exists(input) or androidTools.sha256Of(input) ~= hashes[file] then
            print(tr("cli.modules.package_checksum"))
            return 1
        end
    end
    local runtime = projectlib.joined(destination, "runtime")
    fs.createDirectory(runtime, { makeParents = true })
    for _, file in item.NativeFiles do
        fs.copy(projectlib.joined(source, file), projectlib.joined(runtime, file))
    end
    fs.copy(manifest, projectlib.joined(runtime, "provider.json"))
    fs.writeStringToFile(projectlib.joined(destination, ".engine-module"), `{item.Id}\n{item.Version}\n`)
    print(tr("cli.modules.installed", { module = tr(item.Label), path = runtime }))
    return 0
end
local function install(item: registry.Module): number
    if item.Host ~= nil and item.Host ~= targets.hostPlatform() then
        print(tr("cli.modules.unsupported", { module = tr(item.Label), host = item.Host }))
        return 1
    end
    if item.Kind == "builtin" then
        print(
            tr(
                if available(item) then "cli.modules.present" else "cli.modules.player_missing",
                { module = tr(item.Label) }
            )
        )
        return if available(item) then 0 else 1
    end
    if item.Installer == "android" then
        return androidTools.run()
    end
    if item.Installer ~= "nuget" then
        return 1
    end
    local destination = folder(item)
    fs.createDirectory(destination, { makeParents = true })
    local archive = projectlib.joined(destination, "sdk.nupkg")
    local failed = androidTools.download(item.Url :: string, archive, item.Sha256 :: string)
    if failed then
        print(failed)
        return 1
    end
    -- Extract only after byte verification. This source and hash are pinned in the registry.
    failed = androidTools.unpack(archive, projectlib.joined(destination, "sdk"), nil)
    if failed then
        print(failed)
        return 1
    end
    for _, file in item.RequiredFiles or {} do
        if not fs.exists(projectlib.joined(nativeDirectory(item), file)) then
            return 1
        end
    end
    local inventory: { [string]: string } = {}
    for _, file in item.RequiredFiles or {} do
        inventory[file] = androidTools.sha256Of(projectlib.joined(nativeDirectory(item), file))
    end
    fs.writeStringToFile(projectlib.joined(destination, "inventory.json"), json.serialize(inventory :: any))
    fs.writeStringToFile(projectlib.joined(destination, ".engine-module"), `{item.Id}\n{item.Version}\n`)
    print(tr("cli.modules.installed", { module = tr(item.Label), path = destination }))
    return 0
end
local function remove(item: registry.Module): number
    local destination = folder(item)
    local marker = projectlib.joined(destination, ".engine-module")
    if
        item.Kind == "builtin"
        or item.Installer ~= "nuget"
        or not fs.exists(marker)
        or fs.readFileToString(marker) ~= `{item.Id}\n{item.Version}\n`
    then
        print(tr("cli.modules.not_owned", { module = tr(item.Label) }))
        return 1
    end
    local base = path.format(path.resolve(projectlib.joined(android.toolsFolder(), "modules")))
    local resolved = path.format(path.resolve(destination))
    if string.sub(resolved, 1, #base + 1) ~= base .. "/" and string.sub(resolved, 1, #base + 1) ~= base .. "\\" then
        return 1
    end
    game.removeTree(destination)
    print(tr("cli.modules.removed", { module = tr(item.Label) }))
    return 0
end
return {
    rows = rows,
    enabled = enabled,
    folder = folder,
    available = available,
    verify = verify,
    providerDirectory = providerDirectory,
    nativeDirectory = nativeDirectory,
    install = install,
    installPackage = installPackage,
    remove = remove,
}
