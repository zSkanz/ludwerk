--!strict
--[[
	The dev server: the WebSocket the engine dials into, the watcher that
	notices a save, and the relay between the two (ADR 0035).

	**Only this listens.** The engine opens no port in any profile; it connects
	out to the port this binds, proves it is the process we launched with the
	token on its command line, and receives everything over that one connection.
	Every other client -- the M3 gate's end-to-end test today, the overlay
	console later -- is an ordinary WebSocket client of the same server and gets
	the engine's replies relayed verbatim, the engine's token taken out. What it
	sends reaches the engine only once it has proved it holds the token too, with
	`{ type = "hello", role = "observer", token = ... }` (audit T4).

	The watcher does not trust its own events. `fs.watch` reports the top-level
	entry rather than the changed file, fires two to six times per save, and on
	inotify may not see into a subdirectory at all (U-58) -- so every directory
	is watched individually, an event only marks the tree dirty, and a quiet
	period later the source set is rescanned and diffed. What reaches the engine
	is the real changed-file list.
]]

local crypto = require("@lute/crypto")
local fsRaw = require("@lute/fs")
local json = require("@std/json")
local net = require("@lute/net")
local task = require("@lute/task")

local catalog = require("./catalog")
local project = require("./project")

local tr = catalog.tr

export type Message = { [string]: any }

export type Options = {
    root: string,
    port: number,
    token: string,
    -- Seconds of quiet before a save is acted on. Two to six events per save
    -- would otherwise be two to six world restarts.
    debounce: number?,
    onEngineMessage: ((Message) -> ())?,
    onEngineAttached: ((Message) -> ())?,
    onChanged: (({ string }) -> ())?,
    onLog: ((string) -> ())?,
}

export type Handle = {
    port: number,
    engineAttached: (Handle) -> boolean,
    sendToEngine: (Handle, Message) -> boolean,
    nextRequestId: (Handle) -> number,
    close: (Handle) -> (),
}

local DefaultDebounce = 0.05

--- What buys that another process on this machine -- or a web page, which can
--- open a WebSocket to a loopback port -- cannot drive your engine by finding
--- the port, the whole exposure a loopback listener has. **From the system's
--- random source** (audit T4): `math.random` is a generator a watcher of a few
--- outputs can predict, and it was seeded once per process.
local function mintToken(): string
    local key = crypto.secretbox.keygen()
    local parts: { string } = {}
    for at = 0, buffer.len(key) - 1 do
        table.insert(parts, string.format("%02x", buffer.readu8(key, at)))
    end
    return table.concat(parts)
end

--- The engine's `hello` without its token, which is the one thing about it an
--- observer must not learn (audit T4): it was relayed to every observer and
--- replayed to each one that connected, and a client holding it could claim to
--- be the engine the moment the real one dropped.
local function withoutToken(text: string): string
    local message = json.asObject(json.deserialize(text))
    if message == nil or message.token == nil then
        return text
    end
    message.token = nil
    return json.serialize(message :: any)
end

local function start(options: Options): Handle
    local debounce = options.debounce or DefaultDebounce
    local log = options.onLog or function(_line: string) end

    local engineSocket: net.ServerWebSocket? = nil
    local observers: { net.ServerWebSocket } = {}
    -- The observers that proved they hold the token, and so may ask the
    -- engine for things (audit T4). Any other may listen and nothing more.
    local trusted: { [net.ServerWebSocket]: boolean } = {}
    local nextId = 1

    -- "The engine is attached, and this is who it is" is STATE, not an event.
    -- Relayed only to whoever happened to be connected when it arrived, it is
    -- invisible to every client that attaches afterwards -- including the one
    -- racing the engine at startup, which is most of them. Kept and replayed
    -- on open instead.
    local lastHello: string? = nil

    local function relayToObservers(text: string)
        for _, socket in observers do
            socket:send(text)
        end
    end

    local function handleFromEngine(text: string)
        if string.find(text, '"type":"hello"', 1, true) ~= nil then
            text = withoutToken(text)
            lastHello = text
        end
        relayToObservers(text)

        local decoded = json.deserialize(text)
        local message = json.asObject(decoded)
        if message == nil then
            return
        end

        if message.type == "hello" then
            if options.onEngineAttached ~= nil then
                options.onEngineAttached(message :: any)
            end
            return
        end
        if options.onEngineMessage ~= nil then
            options.onEngineMessage(message :: any)
        end
    end

    local server = net.server.serve({
        hostname = "127.0.0.1",
        port = options.port,
        -- No `handler`. Supplying one at all is what stops the upgrade: the
        -- HTTP handler is consulted first and the WebSocket handlers below
        -- never see the request, even when it returns nil (U-62). There is no
        -- HTTP surface here and there is not meant to be one.
        websocket = {
            message = function(socket, raw)
                local text = if typeof(raw) == "string" then raw else buffer.tostring(raw :: buffer)

                if socket == engineSocket then
                    handleFromEngine(text)
                    return
                end

                local decoded = json.deserialize(text)
                local message = json.asObject(decoded)

                -- The engine identifies itself by carrying the token this
                -- process minted and passed on its command line. Anything else
                -- is an observer: it may listen, and it may ask the engine for
                -- things, but it cannot claim to *be* the engine.
                if
                    engineSocket == nil
                    and message ~= nil
                    and message.type == "hello"
                    and message.role == "engine"
                    and message.token == options.token
                then
                    engineSocket = socket
                    handleFromEngine(text)
                    return
                end

                -- An observer proves itself the same way, and only then is heard.
                if
                    message ~= nil
                    and message.type == "hello"
                    and message.role == "observer"
                    and message.token == options.token
                then
                    trusted[socket] = true
                    return
                end

                if engineSocket ~= nil and message ~= nil and trusted[socket] then
                    (engineSocket :: net.ServerWebSocket):send(text)
                end
            end,
            close = function(socket, _code, _message)
                if socket == engineSocket then
                    engineSocket = nil
                    return
                end
                trusted[socket] = nil
                for index, observer in observers do
                    if observer == socket then
                        table.remove(observers, index)
                        break
                    end
                end
            end,
            open = function(socket)
                -- Everyone starts as an observer. The engine promotes itself by
                -- sending a `hello` carrying the token.
                table.insert(observers, socket)

                -- A client that arrived after the engine did still needs to know
                -- it is there. Without this the attach is a message you had to
                -- be present for, and losing that race is how the hot-reload
                -- gate flaked one run in four.
                if lastHello ~= nil then
                    socket:send(lastHello :: string)
                end
            end,
        },
    })

    -- --- the watcher ---------------------------------------------------------

    -- Declared before the watcher task so it can reach the connection, and
    -- defined after the handle exists.
    local sendReload: ({ string }) -> ()
    local sendAssetChanged: ({ string }) -> ()

    local snapshot = project.sourceSnapshot(options.root)
    -- **Content is snapshotted separately and answers a different verb** (S6.4).
    -- A `.luau` under `src/` is a script and reloads the world; a `.png` under
    -- `content/` is an asset and reloads itself. One verb for both would restart
    -- the game every time somebody saved a texture, which is the opposite of
    -- what a hot reload is for.
    local content = project.contentSnapshot(options.root)
    local dirty = false
    local watched: { fsRaw.WatchHandle } = {}
    local running = true

    for _, dir in project.contentDirectories(options.root) do
        table.insert(
            watched,
            fsRaw.watch(dir, function(_filename, _event)
                dirty = true
            end)
        )
    end

    for _, dir in project.sourceDirectories(options.root) do
        table.insert(
            watched,
            fsRaw.watch(dir, function(_filename, _event)
                -- The event says only that something under here moved. What
                -- changed is decided by the rescan below (U-58).
                dirty = true
            end)
        )
    end
    log(tr("cli.dev.info.watching", { count = #watched }))

    task.spawn(function()
        while running do
            task.wait(debounce)
            if not dirty then
                continue
            end
            dirty = false

            local fresh = project.sourceSnapshot(options.root)
            local changed = project.changedFiles(snapshot, fresh)
            snapshot = fresh

            -- Asked in the same rescan, because one filesystem event does not
            -- say which half of the project it came from.
            local freshContent = project.contentSnapshot(options.root)
            local changedContent = project.changedFiles(content, freshContent)
            content = freshContent
            if #changedContent > 0 then
                sendAssetChanged(changedContent)
            end

            if #changed == 0 then
                -- An editor touched a file without changing it, or wrote a
                -- temporary beside it. Nothing to reload.
                continue
            end

            -- The server sends the reload, rather than leaving it to whoever
            -- supplied a callback. It owns the protocol; `onChanged` is a
            -- notification, and a caller that did not want one should not
            -- thereby get a watcher that notices saves and does nothing.
            local onChanged = options.onChanged
            if onChanged ~= nil then
                onChanged(changed)
            end
            sendReload(changed)
        end
    end)

    local handle = {} :: Handle
    handle.port = server.port

    function handle.engineAttached(_self: Handle): boolean
        return engineSocket ~= nil
    end

    function handle.nextRequestId(_self: Handle): number
        nextId += 1
        return nextId
    end

    function handle.sendToEngine(_self: Handle, message: Message): boolean
        local socket = engineSocket
        if socket == nil then
            return false
        end
        socket:send(json.serialize(json.object(message :: any)))
        return true
    end

    function sendReload(changed: { string })
        nextId += 1
        local socket = engineSocket
        if socket == nil then
            return
        end
        socket:send(json.serialize(json.object({ type = "reload", id = nextId, paths = changed :: any })))
    end

    --- Tells the engine which content files moved, so it can forget what they
    --- loaded (S6.4).
    ---
    --- **Not a reload**, which is the whole distinction: the world keeps
    --- running, its instances keep their ids and their state, and the next
    --- frame reads the changed file off disk. A texture somebody re-exported
    --- appears without a restart.
    function sendAssetChanged(changed: { string })
        nextId += 1
        local socket = engineSocket
        if socket == nil then
            return
        end
        socket:send(json.serialize(json.object({ type = "asset-changed", id = nextId, paths = changed :: any })))
    end

    function handle.close(_self: Handle)
        running = false
        for _, watcher in watched do
            watcher:close()
        end
        server.close()
    end

    return handle
end

return {
    start = start,
    mintToken = mintToken,
}
