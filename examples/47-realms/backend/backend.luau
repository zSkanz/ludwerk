--!strict
--[[
	**A game's own backend**, the reference for ADR 0238 §5: what a game
	that keeps its data in its own service answers the manager, which sends
	every operation of the data service to it with `[data] backend` in
	`servers.toml`.

	    lute examples/47-realms/backend/backend.luau --port=7860 [--state=FILE] [--idle-exit=SECONDS]

	`POST /v1/data` takes one operation as JSON and answers it as JSON, in the
	shapes the engine's own database answers (`engine/app/src/data_ops.cpp`):
	a record's `load` and `write`, a `transact` -- every move or none -- an
	item's `create` and `destroy`, `items`, `balance`, and a shared value's
	`get` and `set` with its version.

	- **`request` is the operation's id.** The manager sends one again, with
	  the same id, when its answer was lost; an operation that changes
	  something is done once per id, and the answer it gave is given again.
	- With `REALMS_BACKEND_TOKEN` set, a request must carry
	  `Authorization: Bearer <token>`.
	- `--state=FILE` keeps everything in a file, written after each change, so
	  a backend started again has what it had. Without it, everything is in
	  memory. A real backend keeps a database; this one keeps the contract.
	- `--idle-exit=SECONDS` ends it that long after the last request, once
	  it has had one: what a test starts beside a manager.
]]

local fs = require("@std/fs")
local json = require("@std/json")
local net = require("@lute/net")
local process = require("@lute/process")
local task = require("@lute/task")
local time = require("@lute/time")

type Object = { [string]: any }

-- --- Arguments --------------------------------------------------------------

local Port = 7860
local StateFile: string? = nil
local IdleExit: number? = nil
for _, argument in process.args do
    local idle = string.match(argument, "^%-%-idle%-exit=(%d+)$")
    if idle then
        IdleExit = tonumber(idle)
    end
    local port = string.match(argument, "^%-%-port=(%d+)$")
    if port then
        Port = tonumber(port) :: number
    end
    local file = string.match(argument, "^%-%-state=(.+)$")
    if file then
        StateFile = file
    end
end
local Token = process.env.REALMS_BACKEND_TOKEN

-- --- What it keeps -----------------------------------------------------------

type State = {
    records: { [string]: { body: string, version: number } },
    items: { [string]: { kind: string, owner: string, fields: string } },
    nextItem: number,
    nextTransaction: number,
    -- owner -> currency -> amount
    balances: { [string]: { [string]: number } },
    -- scope .. "/" .. key -> value and version
    shared: { [string]: { value: string, version: number } },
    -- request id -> the answer it was given
    answered: { [string]: string },
}

local state: State = {
    records = {},
    items = {},
    nextItem = 1,
    nextTransaction = 1,
    balances = {},
    shared = {},
    answered = {},
}

if StateFile and fs.exists(StateFile) then
    local read = json.deserialize(fs.readFileToString(StateFile)) :: any
    if typeof(read) == "table" then
        state = read :: State
    end
end

local function keep()
    if StateFile then
        fs.writeStringToFile(StateFile, json.serialize(state :: any))
    end
end

local function balanceOf(owner: string, currency: string): number
    local held = state.balances[owner]
    return if held then held[currency] or 0 else 0
end

local function add(owner: string, currency: string, amount: number)
    local held = state.balances[owner] or {}
    held[currency] = (held[currency] or 0) + amount
    state.balances[owner] = held
end

-- --- The operations ---------------------------------------------------------

local function refused(reason: string): Object
    return { ok = false, reason = reason }
end

local Changes = { write = true, transact = true, create = true, destroy = true, set = true }

local function run(request: Object): Object
    local op = request.op
    if op == "load" then
        local record = state.records[request.account]
        if record == nil then
            return { ok = true, found = false }
        end
        return { ok = true, found = true, body = record.body, version = record.version }
    elseif op == "write" then
        state.records[request.account] = { body = request.body, version = request.version or 1 }
        return { ok = true }
    elseif op == "transact" then
        local moves = request.moves
        if typeof(moves) ~= "table" or #moves == 0 then
            return refused("Invalid")
        end
        -- **Every move checked, then every move done**: none is done where
        -- one would not be.
        local spending: { [string]: number } = {}
        for _, move in moves do
            if move.item ~= nil then
                local item = state.items[tostring(move.item)]
                if item == nil or item.owner ~= (move.from or "") then
                    return refused("ItemNotThere")
                end
            else
                local amount = move.amount
                if typeof(amount) ~= "number" or amount <= 0 then
                    return refused("Invalid")
                end
                if move.from ~= "" then
                    local key = move.from .. "/" .. move.currency
                    spending[key] = (spending[key] or 0) + amount
                    if balanceOf(move.from, move.currency) < spending[key] then
                        return refused("NotEnough")
                    end
                end
            end
        end
        for _, move in moves do
            if move.item ~= nil then
                local id = tostring(move.item)
                if move.to == "" then
                    state.items[id] = nil
                else
                    state.items[id].owner = move.to
                end
            else
                if move.from ~= "" then
                    add(move.from, move.currency, -move.amount)
                end
                if move.to ~= "" then
                    add(move.to, move.currency, move.amount)
                end
            end
        end
        local id = state.nextTransaction
        state.nextTransaction += 1
        return { ok = true, transaction = id }
    elseif op == "create" then
        local id = state.nextItem
        state.nextItem += 1
        state.items[tostring(id)] = { kind = request.kind, owner = request.owner, fields = request.fields or "" }
        return { ok = true, item = id }
    elseif op == "destroy" then
        local id = tostring(request.item)
        if state.items[id] == nil then
            return refused("ItemNotThere")
        end
        state.items[id] = nil
        return { ok = true }
    elseif op == "items" then
        local list: { { id: number, kind: string, fields: string } } = {}
        for id, item in state.items do
            if item.owner == request.owner then
                table.insert(list, { id = tonumber(id) :: number, kind = item.kind, fields = item.fields })
            end
        end
        table.sort(list, function(a: { id: number }, b: { id: number }): boolean
            return a.id < b.id
        end)
        return { ok = true, items = list }
    elseif op == "balance" then
        return { ok = true, balance = balanceOf(request.owner, request.currency) }
    elseif op == "get" then
        local held = state.shared[request.scope .. "/" .. request.key]
        return { ok = true, value = if held then held.value else "", version = if held then held.version else 0 }
    elseif op == "set" then
        local key = request.scope .. "/" .. request.key
        local held = state.shared[key]
        local version = if held then held.version else 0
        -- **Written only over the version read**: another writer first is a
        -- conflict, answered with the value as it is.
        if request.version ~= version then
            return { ok = false, reason = "Conflict", value = if held then held.value else "", version = version }
        end
        state.shared[key] = { value = request.value, version = version + 1 }
        return { ok = true, value = request.value, version = version + 1 }
    end
    return refused("Invalid")
end

-- --- The server ---------------------------------------------------------------

local lastRequest: time.Instant? = nil

local server = net.server.serve({
    hostname = "127.0.0.1",
    port = Port,
    handler = function(request: net.ReceivedRequest): net.ServerResponse
        lastRequest = time.now()
        if request.method ~= "POST" or request.path ~= "/v1/data" then
            return { status = 404, body = "" }
        end
        if Token and request.headers["authorization"] ~= "Bearer " .. Token then
            return { status = 401, body = "" }
        end
        local ok, decoded = pcall(json.deserialize, request.body)
        if not ok or typeof(decoded) ~= "table" then
            return { status = 400, body = "" }
        end
        local asked = decoded :: Object
        local id = asked.request
        -- **Once per id**: an operation sent again is answered as it was.
        if typeof(id) == "string" and state.answered[id] then
            return { status = 200, body = state.answered[id] }
        end
        local answer = json.serialize(run(asked) :: any)
        if Changes[asked.op] then
            if typeof(id) == "string" then
                state.answered[id] = answer
            end
            keep()
        end
        return { status = 200, headers = { ["content-type"] = "application/json" }, body = answer }
    end,
})

print(`[backend] serving /v1/data on {server.hostname}:{server.port}`)

if IdleExit then
    local idle = IdleExit :: number
    while true do
        task.wait(0.5)
        if lastRequest and time.since(lastRequest :: time.Instant) > idle then
            print("[backend] idle, stopping")
            server.close()
            process.exit(0)
        end
    end
end
