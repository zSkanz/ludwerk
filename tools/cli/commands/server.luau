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
		ludwerk server loadtest <host:port> [path] --bots=N [--seconds=60] [--behavior=wander]
		    [--metrics=<host:port>] [--report=loadtest.json]

	`run` starts the manager when the folder has a `servers.toml`, and one
	dedicated server otherwise; `install` makes that the machine's to start at
	boot -- a systemd unit on Linux, a task that runs at startup on Windows.
	`keygen` makes an administrator's key pair, `connect` reaches a server's
	console with it, and `update` asks a manager to move its worlds to a new
	build, through that same console. `loadtest` sends light bots at a server
	(ADR 0236) -- as many processes of 64 as it takes -- reads the server's
	`/metrics` each second, and writes what both saw.

	**The engine does each of them**: the manager, the handshake and the
	sealing are the server binary's own code, so this only finds it and asks.
]]

local fs = require("@std/fs")
local json = require("@std/json")
local net = require("@std/net")
local path = require("@std/path")
local platform = require("@std/system/platform")
local process = require("@std/process")
local task = require("@std/task")

local brand = require("../brand")
local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

export type Options = {
    action: string?,
    positional: { string },
    bots: string?,
    seconds: string?,
    behavior: string?,
    metrics: string?,
    report: string?,
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

-- The most bots one engine process holds (`MostBotsAProcess`), and the most
-- one command sends from one machine (ADR 0236 §5).
local BotsAProcess = 64
local MostBots = 1000
-- A server takes twenty connections a second from one address; a process
-- starts its bots at ten a second, so the next starts once the last is in.
local BotsJoinedASecond = 10

--- The last of the lines a bots process printed: its totals.
local function lastBotLine(output: string): { [string]: any }?
    local found: { [string]: any }? = nil
    for line in string.gmatch(output, "\30([^\n]+)") do
        local ok, decoded = pcall(json.deserialize, line)
        if ok and typeof(decoded) == "table" and (decoded :: any).type == "bots" then
            found = decoded :: any
        end
    end
    return found
end

--- **A load test**: the bots, the server's numbers while they run, and a
--- report. Its exit code says whether the tick stayed inside its time.
local function loadtest(engine: string, options: Options): number
    local where = options.positional[1]
    local bots = tonumber(options.bots or "")
    if where == nil or bots == nil or bots < 1 or bots > MostBots or bots ~= math.floor(bots) then
        print(tr("cli.server.err.loadtest_usage", { most = MostBots }))
        return 2
    end
    local project = absolute(options.positional[2] or ".")
    local seconds = tonumber(options.seconds or "") or 60
    local behavior = options.behavior or "wander"
    -- A game's own behavior is a file, read by every bots process.
    if string.sub(behavior, -5) == ".luau" then
        behavior = absolute(behavior)
    end
    local metrics = options.metrics or where
    local processes = math.ceil(bots / BotsAProcess)
    local results: { process.ProcessResult } = {}
    local finished = 0
    print(tr("cli.server.loadtest_starting", { bots = bots, processes = processes, address = where }))
    local remaining = bots
    local ramp = 0
    for index = 1, processes do
        local count = math.min(BotsAProcess, remaining)
        remaining -= count
        -- Each process runs as long as the test, less the ramp before it.
        local own = math.max(5, seconds - ramp)
        task.spawn(function()
            results[index] = process.run({
                engine,
                project,
                `--join={where}`,
                `--bots={count}`,
                `--bot-seconds={math.floor(own)}`,
                `--bot-behavior={behavior}`,
            })
            finished += 1
        end)
        local wait = math.ceil(count / BotsJoinedASecond) + 1
        ramp += wait
        if index < processes then
            task.wait(wait)
        end
    end

    -- **The server's side, each second**, while the bots run.
    local samples: { { [string]: any } } = {}
    while finished < processes do
        local ok, response = pcall(function()
            return net.request(`http://{metrics}/metrics`)
        end)
        if ok and (response :: any).status == 200 then
            local parsed, decoded = pcall(json.deserialize, (response :: any).body)
            if parsed and typeof(decoded) == "table" then
                table.insert(samples, decoded :: any)
            end
        end
        task.wait(1)
    end

    local joined, failed, dropped, bytesIn, snapshots, travels, behaviorsFailed = 0, 0, 0, 0, 0, 0, 0
    for index = 1, processes do
        local line = results[index] and lastBotLine(results[index].stdout) or nil
        if line then
            joined += (line.connected :: number?) or 0
            failed += (line.failed :: number?) or 0
            dropped += (line.dropped :: number?) or 0
            bytesIn += (line.bytes_in :: number?) or 0
            snapshots += (line.snapshots :: number?) or 0
            travels += (line.travels :: number?) or 0
            behaviorsFailed += (line.behaviors_failed :: number?) or 0
        else
            failed += math.min(BotsAProcess, bots - (index - 1) * BotsAProcess)
        end
    end

    -- **Each second's own numbers**, from the server's counters: the mean
    -- tick between two samples is the tick time's difference over the
    -- ticks', and the bytes the byte counts' difference. `tick_ms_mean` is
    -- the last minute's, which a person reads, and no use second by second.
    local perSecond: { { [string]: number } } = {}
    local mostPlayers, mostMemory = 0, 0
    for at, sample in samples do
        mostPlayers = math.max(mostPlayers, (sample.players :: number?) or 0)
        mostMemory = math.max(mostMemory, (sample.memory_bytes :: number?) or 0)
        local before = samples[at - 1]
        if before then
            local ticks = ((sample.ticks_total :: number?) or 0) - ((before.ticks_total :: number?) or 0)
            local ms = ((sample.tick_ms_total :: number?) or 0) - ((before.tick_ms_total :: number?) or 0)
            local second: { [string]: number } = {
                players = (sample.players :: number?) or 0,
                ticks = ticks,
                tick_ms = if ticks > 0 then ms / ticks else 0,
                bytes_out = ((sample.wire_bytes_sent :: number?) or 0) - ((before.wire_bytes_sent :: number?) or 0),
                bytes_in = ((sample.wire_bytes_received :: number?) or 0)
                    - ((before.wire_bytes_received :: number?) or 0),
            }
            table.insert(perSecond, second)
        end
    end
    -- **The plateau**: the seconds every bot that got in was in. Joining and
    -- leaving cost what they cost -- a newcomer is sent the whole world --
    -- and are reported apart, as the peaks.
    local held: { { [string]: number } } = {}
    for _, second in perSecond do
        if second.players >= mostPlayers and mostPlayers > 0 then
            table.insert(held, second)
        end
    end
    local function meanOf(rows: { { [string]: number } }, field: string): number
        local sum = 0
        for _, row in rows do
            sum += row[field]
        end
        return if #rows > 0 then sum / #rows else 0
    end
    local function mostOf(rows: { { [string]: number } }, field: string): number
        local most = 0
        for _, row in rows do
            most = math.max(most, row[field])
        end
        return most
    end
    local heldTickMean = meanOf(held, "tick_ms")
    local heldTickWorst = mostOf(held, "tick_ms")
    local heldTicks = meanOf(held, "ticks")
    local report = {
        bots = bots,
        processes = processes,
        behavior = behavior,
        seconds = seconds,
        joined = joined,
        failed = failed,
        dropped = dropped,
        travels = travels,
        behaviors_failed = behaviorsFailed,
        bytes_in = bytesIn,
        snapshots = snapshots,
        server_players_most = mostPlayers,
        server_memory_bytes_most = mostMemory,
        held_seconds = #held,
        held_tick_ms_mean = heldTickMean,
        held_tick_ms_worst_second = heldTickWorst,
        held_ticks_a_second = heldTicks,
        held_bytes_out_a_second = meanOf(held, "bytes_out"),
        held_bytes_in_a_second = meanOf(held, "bytes_in"),
        peak_bytes_out_a_second = mostOf(perSecond, "bytes_out"),
        each_second = perSecond,
        samples = samples,
    }
    print(tr("cli.server.loadtest_bots", { joined = joined, bots = bots, failed = failed, dropped = dropped }))
    -- What the bots were told when a join failed, or why a behavior stopped,
    -- each once: a server's per-address limit, a full server, another
    -- version, a behavior's error.
    local seen: { [string]: boolean } = {}
    for index = 1, processes do
        local result = results[index]
        if result then
            for line in string.gmatch(result.stdout .. "\n" .. result.stderr, "[^\n]+") do
                local said = string.find(line, "[warn]", 1, true) or string.find(line, "[error]", 1, true)
                if said and not seen[line] then
                    seen[line] = true
                    print(line)
                end
            end
        end
    end
    if #held > 0 then
        print(tr("cli.server.loadtest_server", {
            players = mostPlayers,
            seconds = #held,
            mean = string.format("%.2f", heldTickMean),
            worst = string.format("%.2f", heldTickWorst),
            rate = string.format("%.1f", heldTicks),
            out = string.format("%.1f", meanOf(held, "bytes_out") / 1024),
            into = string.format("%.1f", meanOf(held, "bytes_in") / 1024),
            peak = string.format("%.1f", mostOf(perSecond, "bytes_out") / 1024),
            memory = math.floor(mostMemory / (1024 * 1024)),
        }))
    else
        print(tr("cli.server.loadtest_no_metrics", { address = metrics }))
    end
    local file = absolute(options.report or "loadtest.json")
    fs.writeStringToFile(file, json.serialize(report :: any))
    print(tr("cli.server.loadtest_report", { path = file }))
    -- **Passed**: everybody got in and stayed, every behavior ran, and no
    -- second of the plateau took longer than its sixty ticks may.
    local inside = #held > 0 and heldTickWorst < 1000 / 60
    return if inside and failed == 0 and dropped == 0 and behaviorsFailed == 0 then 0 else 1
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

    if action == "loadtest" then
        return loadtest(engine, options)
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
