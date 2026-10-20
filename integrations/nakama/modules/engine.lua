--[[
	Ludwerk's runtime module for Nakama (ADR 0242, as amended 2026-10-20).
	Apache-2.0. Put it in Nakama's `modules` folder; nothing else is needed.

	Two RPCs:

	- `engine_whoami`, called by the game's front door with a player's own
	  session token as its bearer: Nakama has checked the session, and this
	  answers whose it is. The front door signs the engine's own ticket for
	  that user; no key is shared between Nakama and the game.

	- `engine_data`, called by the game's manager with the server's HTTP key
	  (`?http_key=...&unwrap`): the data service's contract (ADR 0238 §5) --
	  the same JSON the engine's own database reads and answers -- kept in
	  Nakama's storage, owned by the system. A change is one `storage_write`
	  of everything it touches, each with the version it was read at, and of
	  its request's id with version "*": Nakama writes the batch whole or not
	  at all, so a request sent twice is done once, and two worlds changing
	  one thing at once are told apart. A batch refused for a version is read
	  and tried again.
]]

local nk = require("nakama")

local Records = "engine_records"
local Items = "engine_items"
local Owned = "engine_owned"
local Balances = "engine_balances"
local Shared = "engine_shared"
local Counters = "engine_counters"
local Requests = "engine_requests"
local Tries = 8

-- --- engine_whoami -----------------------------------------------------------

local function whoami(context, _payload)
    if context.user_id == nil or context.user_id == "" then
        error({ "a player's session is the bearer of this call", 16 })
    end
    return nk.json_encode({ user_id = context.user_id, username = context.username })
end
nk.register_rpc(whoami, "engine_whoami")

-- --- Storage, owned by the system -------------------------------------------

local function id(collection, key)
    return { collection = collection, key = key }
end

-- The objects asked for, by "collection/key", each with its value and the
-- version Nakama keeps -- "*" for one not there yet, which a write then
-- creates only if it still is not.
local function read(ids)
    local found = {}
    for _, one in ipairs(ids) do
        found[one.collection .. "/" .. one.key] = { value = nil, version = "*" }
    end
    if #ids > 0 then
        local objects = nk.storage_read(ids)
        for _, object in ipairs(objects) do
            found[object.collection .. "/" .. object.key] = { value = object.value, version = object.version }
        end
    end
    return found
end

local function write(found, collection, key, value)
    local held = found[collection .. "/" .. key]
    return {
        collection = collection,
        key = key,
        value = value,
        version = held and held.version or "*",
        permission_read = 0,
        permission_write = 0,
    }
end

-- **A change, once per request id**: `change` reads what it needs and
-- returns the writes and the answer; they are written with the request's
-- own record, and a batch refused is read again -- unless the refusal was
-- the request's record, written already, whose answer is the answer.
local function once(request, change)
    local key = request.request
    for _ = 1, Tries do
        if key ~= nil then
            local done = nk.storage_read({ id(Requests, key) })
            if #done > 0 then
                return done[1].value.answer
            end
        end
        local writes, answer = change()
        if writes == nil then
            return answer
        end
        if key ~= nil then
            table.insert(writes, {
                collection = Requests,
                key = key,
                value = { answer = answer },
                version = "*",
                permission_read = 0,
                permission_write = 0,
            })
        end
        local ok = pcall(nk.storage_write, writes)
        if ok then
            return answer
        end
    end
    return { ok = false, reason = "Failed" }
end

local function refused(reason)
    return { ok = false, reason = reason }
end

-- --- The operations ----------------------------------------------------------

local Ops = {}

function Ops.load(request)
    local found = read({ id(Records, request.account) })[Records .. "/" .. request.account]
    if found.value == nil then
        return { ok = true, found = false }
    end
    return { ok = true, found = true, body = found.value.body, version = found.value.version }
end

function Ops.write(request)
    return once(request, function()
        local found = read({ id(Records, request.account) })
        return { write(found, Records, request.account, { body = request.body, version = request.version or 1 }) }, {
            ok = true,
        }
    end)
end

function Ops.items(request)
    local owned = read({ id(Owned, request.owner) })[Owned .. "/" .. request.owner].value
    local list = {}
    if owned ~= nil then
        local ids = {}
        for _, item in ipairs(owned.ids) do
            table.insert(ids, id(Items, tostring(item)))
        end
        local found = read(ids)
        for _, item in ipairs(owned.ids) do
            local held = found[Items .. "/" .. tostring(item)]
            if held.value ~= nil then
                table.insert(list, { id = item, kind = held.value.kind, fields = held.value.fields })
            end
        end
    end
    return { ok = true, items = list }
end

function Ops.balance(request)
    local held = read({ id(Balances, request.owner) })[Balances .. "/" .. request.owner].value
    return { ok = true, balance = held and held[request.currency] or 0 }
end

function Ops.get(request)
    local key = request.scope .. "/" .. request.key
    local held = read({ id(Shared, key) })[Shared .. "/" .. key].value
    return { ok = true, value = held and held.value or "", version = held and held.version or 0 }
end

function Ops.set(request)
    local key = request.scope .. "/" .. request.key
    return once(request, function()
        local found = read({ id(Shared, key) })
        local held = found[Shared .. "/" .. key].value
        local version = held and held.version or 0
        if request.version ~= version then
            return nil, { ok = false, reason = "Conflict", value = held and held.value or "", version = version }
        end
        return { write(found, Shared, key, { value = request.value, version = version + 1 }) }, {
            ok = true,
            value = request.value,
            version = version + 1,
        }
    end)
end

-- An owner's list of items, with `item` taken out or put in.
local function without(list, item)
    local out = {}
    for _, one in ipairs(list or {}) do
        if one ~= item then
            table.insert(out, one)
        end
    end
    return out
end

function Ops.create(request)
    return once(request, function()
        local found = read({ id(Counters, "item"), id(Owned, request.owner) })
        local counter = found[Counters .. "/item"].value
        local item = counter and counter.next or 1
        local owned = found[Owned .. "/" .. request.owner].value
        local ids = without(owned and owned.ids, item)
        table.insert(ids, item)
        return {
            write(found, Counters, "item", { next = item + 1 }),
            write(found, Owned, request.owner, { ids = ids }),
            {
                collection = Items,
                key = tostring(item),
                value = { kind = request.kind, owner = request.owner, fields = request.fields or "" },
                version = "*",
                permission_read = 0,
                permission_write = 0,
            },
        }, { ok = true, item = item }
    end)
end

function Ops.destroy(request)
    return once(request, function()
        local key = tostring(request.item)
        local found = read({ id(Items, key) })
        local item = found[Items .. "/" .. key].value
        if item == nil then
            return nil, refused("ItemNotThere")
        end
        local owners = read({ id(Owned, item.owner) })
        local owned = owners[Owned .. "/" .. item.owner].value
        -- An item destroyed is written as nothing, at the version read.
        return {
            write(found, Items, key, { gone = true }),
            write(owners, Owned, item.owner, { ids = without(owned and owned.ids, request.item) }),
        }, { ok = true }
    end)
end

function Ops.transact(request)
    local moves = request.moves
    if type(moves) ~= "table" or #moves == 0 then
        return refused("Invalid")
    end
    return once(request, function()
        -- **Everything it touches, read at once**: the items, their owners'
        -- lists, every balance, and the transaction counter.
        local ids = { id(Counters, "transaction") }
        local seen = {}
        local function want(collection, key)
            local name = collection .. "/" .. key
            if not seen[name] then
                seen[name] = true
                table.insert(ids, id(collection, key))
            end
        end
        for _, move in ipairs(moves) do
            if move.item ~= nil then
                want(Items, tostring(move.item))
                if move.from ~= "" then
                    want(Owned, move.from)
                end
                if move.to ~= "" then
                    want(Owned, move.to)
                end
            else
                if move.from ~= "" then
                    want(Balances, move.from)
                end
                if move.to ~= "" then
                    want(Balances, move.to)
                end
            end
        end
        local found = read(ids)
        local values = {}
        for name, held in pairs(found) do
            values[name] = held.value
        end
        -- **Every move checked against what was read, then every move made**
        -- on the copies: none is made where one would not be.
        for _, move in ipairs(moves) do
            if move.item ~= nil then
                local name = Items .. "/" .. tostring(move.item)
                local item = values[name]
                if item == nil or item.gone or item.owner ~= move.from then
                    return nil, refused("ItemNotThere")
                end
                local moved = { kind = item.kind, fields = item.fields, owner = move.to }
                if move.to == "" then
                    moved = { gone = true }
                end
                values[name] = moved
                if move.from ~= "" then
                    local owned = values[Owned .. "/" .. move.from]
                    values[Owned .. "/" .. move.from] = { ids = without(owned and owned.ids, move.item) }
                end
                if move.to ~= "" then
                    local owned = values[Owned .. "/" .. move.to]
                    local list = without(owned and owned.ids, move.item)
                    table.insert(list, move.item)
                    values[Owned .. "/" .. move.to] = { ids = list }
                end
            else
                local amount = move.amount
                if type(amount) ~= "number" or amount <= 0 then
                    return nil, refused("Invalid")
                end
                if move.from ~= "" then
                    local name = Balances .. "/" .. move.from
                    local held = values[name] or {}
                    local have = held[move.currency] or 0
                    if have < amount then
                        return nil, refused("NotEnough")
                    end
                    held[move.currency] = have - amount
                    values[name] = held
                end
                if move.to ~= "" then
                    local name = Balances .. "/" .. move.to
                    local held = values[name] or {}
                    held[move.currency] = (held[move.currency] or 0) + amount
                    values[name] = held
                end
            end
        end
        local counter = values[Counters .. "/transaction"]
        local transaction = counter and counter.next or 1
        values[Counters .. "/transaction"] = { next = transaction + 1 }
        local writes = {}
        for _, one in ipairs(ids) do
            table.insert(writes, write(found, one.collection, one.key, values[one.collection .. "/" .. one.key]))
        end
        return writes, { ok = true, transaction = transaction }
    end)
end

-- --- engine_data -----------------------------------------------------------------

local function data(context, payload)
    -- **The manager's alone**: with the server's HTTP key there is no user;
    -- a player's session calling it is refused.
    if context.user_id ~= nil and context.user_id ~= "" then
        error({ "engine_data is the game's servers', not a player's", 7 })
    end
    local request = nk.json_decode(payload)
    local op = Ops[request.op]
    if op == nil then
        return nk.json_encode(refused("Invalid"))
    end
    return nk.json_encode(op(request))
end
nk.register_rpc(data, "engine_data")
