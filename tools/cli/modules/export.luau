--!strict
-- Native integration payloads are separate from the platform player (ADR 0188).
local catalog = require("../catalog")
local fs = require("@std/fs")
local manager = require("./manager")
local path = require("@std/path")
local platform = require("@std/system/platform")
local projectlib = require("../project")
local registry = require("./registry")
local targets = require("../export/targets")

export type Payload = { Item: registry.Module, Directory: string, GameConfig: string? }
local function field(config: any, key: string): any
    local value = config
    for _, part in string.split(key, ".") do
        value = if typeof(value) == "table" then value[part] else nil
    end
    return value
end
local function prepare(project: projectlib.Project, target: targets.Target, executable: string): ({ Payload }?, string?)
    local payloads: { Payload } = {}
    for _, item in registry.All do
        if item.Integration == nil or not manager.enabled(project.config, item.Integration) then
            continue
        end
        if table.find(item.Targets, target.name) == nil then
            continue
        end
        local directory = manager.providerDirectory(item)
        if directory == nil then
            return nil, catalog.tr("cli.modules.provider_missing", { module = catalog.tr(item.Label) })
        end
        local configuration = field(project.config, `integrations.{item.Integration}.configuration`)
        if typeof(configuration) ~= "string" or configuration == "" then
            return nil, catalog.tr("cli.modules.configuration_missing", { module = catalog.tr(item.Label) })
        end
        local gameConfig: string? = nil
        if item.GameConfigKey ~= nil then
            local authored = field(project.config, item.GameConfigKey)
            if
                typeof(authored) ~= "string"
                or authored == ""
                or (string.match(authored, "^[/\\]") ~= nil or string.match(authored, "^%a:") ~= nil)
            then
                return nil, catalog.tr("cli.modules.config_missing")
            end
            local root = path.format(path.resolve(project.root))
            local resolved = path.format(path.resolve(projectlib.joined(project.root, authored)))
            local fold = function(value: string): string
                local normalized = string.gsub(value, "\\", "/")
                return if platform.win32 then string.lower(normalized) else normalized
            end
            if string.sub(fold(resolved), 1, #root + 1) ~= fold(root) .. "/" or not fs.exists(resolved) then
                return nil, catalog.tr("cli.modules.config_missing")
            end
            local xml = fs.readFileToString(resolved)
            local name = string.match(xml, '<Executable%s+[^>]-Name%s*=%s*"([^"]+)"')
                or string.match(xml, "<Executable%s+[^>]-Name%s*=%s*'([^']+)'")
            if name ~= executable then
                return nil, catalog.tr("cli.modules.executable_mismatch", { expected = executable })
            end
            gameConfig = resolved
        end
        table.insert(payloads, { Item = item, Directory = directory, GameConfig = gameConfig })
    end
    return payloads, nil
end
local function deploy(payloads: { Payload }, output: string)
    for _, payload in payloads do
        local destination = projectlib.joined(output, "integrations", payload.Item.Integration :: string)
        fs.createDirectory(destination, { makeParents = true })
        for _, file in payload.Item.NativeFiles or {} do
            fs.copy(projectlib.joined(payload.Directory, file), projectlib.joined(destination, file))
        end
        if payload.GameConfig ~= nil then
            fs.copy(payload.GameConfig, projectlib.joined(output, payload.Item.GameConfigName :: string))
        end
    end
end
return { prepare = prepare, deploy = deploy }
