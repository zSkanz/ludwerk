--!strict
-- Platform descriptions are data; installers and exporters consume this registry.
export type Module = {
    Id: string,
    Label: string,
    Kind: string,
    Targets: { string },
    Dependencies: { string },
    Integration: string?,
    Installer: string?,
    Host: string?,
    Version: string?,
    Url: string?,
    Sha256: string?,
    Bytes: number?,
    NativeDirectory: string?,
    Payload: string?,
    NativeFiles: { string }?,
    -- **The vendor's own files a game needs beside the provider** (ADR 0188):
    -- never in the engine's module archive, which carries the engine's layer
    -- and nothing of theirs. Each is found in the developer's installed SDK
    -- (`From`, under its native directory) and copied into the exported game
    -- (`To`) when it is exported.
    VendorFiles: { { From: string, To: string } }?,
    RequiredFiles: { string }?,
    GameConfigKey: string?,
    GameConfigName: string?,
    Settings: { { Key: string, Label: string } }?,
}
local Modules: { Module } = {
    {
        Id = "windows",
        Label = "cli.modules.windows",
        Kind = "builtin",
        Targets = { "windows", "windows-server" },
        Dependencies = {},
    },
    {
        Id = "linux",
        Label = "cli.modules.linux",
        Kind = "builtin",
        Targets = { "linux", "linux-server" },
        Dependencies = {},
    },
    {
        Id = "android",
        Label = "cli.modules.android",
        Kind = "platform",
        Targets = { "android" },
        Dependencies = {},
        Installer = "android",
    },
    {
        Id = "microsoft-gdk",
        Label = "cli.modules.microsoft_gdk",
        Kind = "integration",
        Targets = { "windows" },
        Integration = "xbox",
        Dependencies = {},
        Installer = "nuget",
        Host = "windows",
        Settings = {
            { Key = "integrations.xbox.configuration", Label = "cli.modules.scid" },
            { Key = "export.microsoft.config", Label = "cli.modules.game_config" },
        },
        Payload = "windows-x64",
        NativeFiles = { "engine_xbox.dll" },
        VendorFiles = {
            { From = "bin/x64/xgameruntime.dll", To = "xgameruntime.dll" },
            { From = "bin/x64/libHttpClient.dll", To = "libHttpClient.dll" },
        },
        RequiredFiles = {
            "include/XGameRuntimeInit.h",
            "lib/x64/xgameruntime.lib",
            "lib/x64/Microsoft.Xbox.Services.143.C.lib",
            "bin/x64/xgameruntime.dll",
            "bin/x64/libHttpClient.dll",
        },
        GameConfigKey = "export.microsoft.config",
        GameConfigName = "MicrosoftGame.config",
        Version = "2604.5.7903",
        NativeDirectory = "native/260405/windows",
        Url = "https://api.nuget.org/v3-flatcontainer/microsoft.gdk.windows/2604.5.7903/microsoft.gdk.windows.2604.5.7903.nupkg",
        Sha256 = "6679f5ae3f1ed873676f9ad363eed4fd754ffcf795628490e916ca8a9058a4e3",
        Bytes = 141346554,
    },
}
local function find(id: string): Module?
    for _, item in Modules do
        if item.Id == id then
            return item
        end
    end
    return nil
end
-- Dependency order, with missing nodes and cycles rejected before any download.
local function resolve(id: string): ({ Module }?, string?)
    local ordered: { Module } = {}
    local visited: { [string]: number } = {}
    local function visit(name: string): string?
        if visited[name] == 2 then
            return nil
        end
        if visited[name] == 1 then
            return "DependencyCycle"
        end
        local item = find(name)
        if item == nil then
            return "UnknownModule"
        end
        visited[name] = 1
        for _, dependency in item.Dependencies do
            local failure = visit(dependency)
            if failure then
                return failure
            end
        end
        visited[name] = 2
        table.insert(ordered, item)
        return nil
    end
    local failure = visit(id)
    return if failure then nil else ordered, failure
end
return { All = Modules, find = find, resolve = resolve }
