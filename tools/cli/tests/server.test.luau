--!strict
--[[
	`ludwerk server`: what it runs, and what it installs.

	The command itself starts processes and writes system files, which a test
	has no business doing; what it would do is a pure function of the folder and
	the flags, and that is what these check -- the manager for a folder with a
	`servers.toml`, one server otherwise, and the unit and the task that start
	the same command with the machine.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local path = require("@std/path")
local test = require("@std/test")

local server = require("../commands/server")

local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    local folder = `{base}/scratch-server-{name}`
    if fs.exists(folder) then
        fs.removeDirectory(folder, { recursive = true })
    end
    fs.createDirectory(folder, { makeParents = true })
    return folder
end

test.suite("server", function(suite)
    suite:case("a folder with servers.toml runs the manager, any other one server", function()
        local managed = scratch("server-managed")
        fs.writeStringToFile(path.format(path.join(managed, "servers.toml")), '[[world]]\nname = "town"\n')
        local command = server.serverCommand("engine-host", managed, 7777, true, true)
        assert.eq(#command, 2)
        assert.eq(command[1], "engine-host")
        assert.strContains(command[2], "--manager=")
        assert.strContains(command[2], "servers.toml")

        local single = scratch("server-single")
        local served = server.serverCommand("engine-host", single, 7790, true, false)
        assert.eq(served[2], single)
        assert.eq(served[3], "--serve=7790")
        assert.eq(served[4], "--admin")
        assert.eq(#served, 4)
        fs.removeDirectory(managed, { recursive = true })
        fs.removeDirectory(single, { recursive = true })
    end)

    suite:case("the unit runs the command in its folder, again after a fall, as the user who installed it", function()
        local unit = server.unitText(
            "realms-servers",
            "/srv/realms",
            { "/srv/realms/engine-host", "--manager=/srv/realms/servers.toml" },
            "game"
        )
        assert.strContains(unit, "WorkingDirectory=/srv/realms")
        assert.strContains(unit, "ExecStart=/srv/realms/engine-host --manager=/srv/realms/servers.toml")
        assert.strContains(unit, "Restart=on-failure")
        assert.strContains(unit, "User=game")
        assert.strContains(unit, "WantedBy=multi-user.target")
        -- Root is systemd's default, never written; a path with a space is quoted.
        local asRoot = server.unitText("x", "/srv/my game", { "/srv/my game/engine-host" }, "root")
        assert.eq(string.find(asRoot, "User=", 1, true), nil)
        assert.strContains(asRoot, 'ExecStart="/srv/my game/engine-host"')
    end)

    suite:case("the Windows task starts the same command at startup, as the system", function()
        local arguments = server.taskArguments(
            "realms-servers",
            { "C:\\Games\\Realms\\engine-host.exe", "--manager=C:\\Games\\Realms\\servers.toml" }
        )
        assert.eq(arguments[1], "schtasks")
        assert.eq(arguments[4], "realms-servers")
        assert.eq(arguments[6], "C:\\Games\\Realms\\engine-host.exe --manager=C:\\Games\\Realms\\servers.toml")
        assert.eq(table.find(arguments, "ONSTART") ~= nil, true)
        assert.eq(table.find(arguments, "SYSTEM") ~= nil, true)
    end)

    suite:case("an administrator reaches the game's port plus one unless told another", function()
        local host, port = server.hostAndPort("play.example.com", nil)
        assert.eq(host, "play.example.com")
        assert.eq(port, 7778)
        host, port = server.hostAndPort("play.example.com:9001", nil)
        assert.eq(host, "play.example.com")
        assert.eq(port, 9001)
        host, port = server.hostAndPort("10.0.0.4", "7801")
        assert.eq(port, 7801)
    end)
end)
