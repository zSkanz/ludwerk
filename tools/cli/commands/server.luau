--!strict
--[[
	`ludwerk server` -- an online game's servers, from the folder they run in
	(ADRs 0219 and 0220).

		ludwerk server run [path] [--port=7777] [--admin] [--health]
		ludwerk server install [path] [--name=<service>] [--port=7777]
		ludwerk server uninstall [--name=<service>]
		ludwerk server keygen --name=ana [--out=<folder>]
		ludwerk server connect <host>[:port] --key=ana.key [--server-key=<hex>] [--follow] [--command=<line>]
		ludwerk server update <folder> [--key=ana.key] [--port=7778]

	`run` starts the manager when the folder has a `servers.toml`, and one
	dedicated server otherwise; `install` makes that the machine's to start at
	boot -- a systemd unit on Linux, a task that runs at startup on Windows.
	`keygen` makes an administrator's key pair, `connect` reaches a server's
	console with it, and `update` asks a manager to move its worlds to a new
	build, through that same console.

	**The engine does each of them**: the manager, the handshake and the
	sealing are the server binary's own code, so this only finds it and asks.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local platform = require("@std/system/platform")
local process = require("@std/process")

local brand = require("../brand")
local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

export type Options = {
    action: string?,
    positional: { string },
    port: string?,
    name: string?,
    out: string?,
    key: string?,
    serverKey: string?,
    command: string?,
    follow: boolean,
    admin: boolean,
    health: boolean,
}

local DefaultGamePort = 7777

local function absolute(file: string): string
    return path.format(path.resolve(file))
end

--- What starts the folder's servers: the manager when it has a
--- `servers.toml`, else the game served on `port`.
local function serverCommand(engine: string, folder: string, port: number, admin: boolean, health: boolean): { string }
    local servers = path.format(path.join(folder, "servers.toml"))
    if fs.exists(servers) then
        return { engine, `--manager={servers}` }
    end
    local command = { engine, folder, `--serve={port}` }
    if admin then
        table.insert(command, "--admin")
    end
    if health then
        table.insert(command, "--health")
    end
    return command
end

--- One argument as a shell or a unit file reads it: quoted where it needs to be.
local function quoted(argument: string): string
    if string.find(argument, '[%s"]') == nil then
        return argument
    end
    local escaped = string.gsub(argument, '"', '\\"')
    return `"{escaped}"`
end

--- **The systemd unit**: the servers in their folder, started with the
--- machine and again after a fall, stopped with the time a `stop` takes.
local function unitText(name: string, folder: string, command: { string }, user: string?): string
    local quotedCommand: { string } = {}
    for _, argument in command do
        table.insert(quotedCommand, quoted(argument))
    end
    local lines = {
        "[Unit]",
        `Description={name} ({brand.read().Name} game servers)`,
        "After=network-online.target",
        "Wants=network-online.target",
        "",
        "[Service]",
        "Type=simple",
        `WorkingDirectory={folder}`,
        `ExecStart={table.concat(quotedCommand, " ")}`,
        "Restart=on-failure",
        "RestartSec=5",
        "KillSignal=SIGTERM",
        "TimeoutStopSec=600",
    }
    if user ~= nil and user ~= "" and user ~= "root" then
        table.insert(lines, `User={user}`)
    end
    table.insert(lines, "")
    table.insert(lines, "[Install]")
    table.insert(lines, "WantedBy=multi-user.target")
    table.insert(lines, "")
    return table.concat(lines, "\n")
end

--- **The Windows task**: `schtasks` arguments that run the command at
--- startup, as the system, whoever is signed in.
local function taskArguments(name: string, command: { string }): { string }
    local quotedCommand: { string } = {}
    for _, argument in command do
        table.insert(quotedCommand, quoted(argument))
    end
    return {
        "schtasks",
        "/Create",
        "/TN",
        name,
        "/TR",
        table.concat(quotedCommand, " "),
        "/SC",
        "ONSTART",
        "/RU",
        "SYSTEM",
        "/RL",
        "HIGHEST",
        "/F",
    }
end

--- `host`, `host:port`: the port given, else the game's plus one.
local function hostAndPort(where: string, port: string?): (string, number)
    local host, written = string.match(where, "^(.*):(%d+)$")
    if host ~= nil and written ~= nil then
        return host, tonumber(written) :: number
    end
    return where, tonumber(port or "") or DefaultGamePort + 1
end

local function serviceName(options: Options, folder: string): string
    if options.name ~= nil and options.name ~= "true" then
        return options.name
    end
    local base = string.match(folder, "([^/\\]+)[/\\]*$") or "game"
    return `{string.lower((string.gsub(base, "[^%w%-_]", "-")))}-servers`
end

local function run(options: Options): number
    local engine = projectlib.findEngine()
    if engine == nil then
        print(tr("cli.err.no_engine"))
        return 3
    end
    local action = options.action
    local port = tonumber(options.port or "") or DefaultGamePort

    if action == "run" then
        local folder = absolute(options.positional[1] or ".")
        local result =
            process.run(serverCommand(engine, folder, port, options.admin, options.health), { stdio = "inherit" })
        return if result.ok then 0 else result.exitcode
    end

    if action == "install" then
        local folder = absolute(options.positional[1] or ".")
        local name = serviceName(options, folder)
        local command = serverCommand(engine, folder, port, options.admin, options.health)
        if platform.win32 then
            local result = process.run(taskArguments(name, command), { stdio = "inherit" })
            if not result.ok then
                print(tr("cli.server.err.install_windows", { name = name }))
                return 1
            end
            print(tr("cli.server.installed_windows", { name = name }))
            return 0
        end
        local unit = unitText(name, folder, command, process.env.SUDO_USER or process.env.USER)
        local file = `/etc/systemd/system/{name}.service`
        local written = pcall(fs.writeStringToFile, file, unit)
        if not written then
            print(unit)
            print(tr("cli.server.err.install_linux", { file = file }))
            return 1
        end
        process.run({ "systemctl", "daemon-reload" }, { stdio = "inherit" })
        local enabled = process.run({ "systemctl", "enable", "--now", name }, { stdio = "inherit" })
        if not enabled.ok then
            return 1
        end
        print(tr("cli.server.installed_linux", { name = name, file = file }))
        return 0
    end

    if action == "uninstall" then
        local name = serviceName(options, absolute(options.positional[1] or "."))
        if platform.win32 then
            local result = process.run({ "schtasks", "/Delete", "/TN", name, "/F" }, { stdio = "inherit" })
            return if result.ok then 0 else 1
        end
        process.run({ "systemctl", "disable", "--now", name }, { stdio = "inherit" })
        local file = `/etc/systemd/system/{name}.service`
        if fs.exists(file) and not pcall(fs.remove, file) then
            print(tr("cli.server.err.install_linux", { file = file }))
            return 1
        end
        process.run({ "systemctl", "daemon-reload" }, { stdio = "inherit" })
        print(tr("cli.server.uninstalled", { name = name }))
        return 0
    end

    if action == "keygen" then
        if options.name == nil or options.name == "true" then
            print(tr("cli.server.err.usage"))
            return 2
        end
        local file = absolute(path.format(path.join(options.out or ".", `{options.name}.key`)))
        local result = process.run({ engine, `--admin-keygen={file}` }, { stdio = "inherit" })
        if not result.ok then
            return 1
        end
        print(tr("cli.server.keygen_done", { path = file, name = options.name }))
        return 0
    end

    if action == "connect" or action == "update" then
        local where = if action == "connect" then options.positional[1] else "127.0.0.1"
        local key = options.key
        if where == nil or key == nil or (action == "update" and options.positional[1] == nil) then
            print(tr("cli.server.err.usage"))
            return 2
        end
        local host, adminPort = hostAndPort(where, options.port)
        local command = { engine, `--admin-connect={host}:{adminPort}`, `--admin-key={absolute(key)}` }
        if options.serverKey ~= nil then
            table.insert(command, `--server-key={options.serverKey}`)
        end
        if options.follow then
            table.insert(command, "--follow")
        end
        if action == "update" then
            table.insert(command, `--command=update {absolute(options.positional[1])}`)
        elseif options.command ~= nil then
            table.insert(command, `--command={options.command}`)
        end
        local result = process.run(command, { stdio = "inherit" })
        return if result.ok then 0 else result.exitcode
    end

    print(tr("cli.server.err.usage"))
    return 2
end

return {
    run = run,
    serverCommand = serverCommand,
    unitText = unitText,
    taskArguments = taskArguments,
    hostAndPort = hostAndPort,
}
