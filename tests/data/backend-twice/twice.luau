--!strict
-- **The same request twice is one trade** (ADR 0238 §5): what the manager
-- does when an answer is lost on its way back -- it sends the request again,
-- with the same id. The backend answers it as it did and moves nothing more;
-- a new id moves again.
--
--     lute tests/data/backend-twice/twice.luau --port=N
--     lute tests/data/backend-twice/twice.luau --url=ENDPOINT [--owner=NAME]
--
-- `--url` asks another backend at its own address -- the game's Nakama, by
-- its module's RPC -- and `--owner` names whose gold moves, so a backend
-- that keeps what it had is asked about an owner of this run's own.

local json = require("@std/json")
local net = require("@lute/net")
local process = require("@lute/process")
local task = require("@lute/task")

local Url = ""
local Owner = "twice"
for _, argument in process.args do
    local port = string.match(argument, "^%-%-port=(%d+)$")
    if port then
        Url = `http://127.0.0.1:{port}/v1/data`
    end
    local url = string.match(argument, "^%-%-url=(.+)$")
    if url then
        Url = url
    end
    local owner = string.match(argument, "^%-%-owner=(.+)$")
    if owner then
        Owner = owner
    end
end

local function ask(request: { [string]: any }): { [string]: any }?
    local metadata: net.Metadata = { method = "POST", body = json.serialize(request :: any) }
    local ok, response = pcall(net.client.request, Url, metadata)
    if not ok or (response :: net.Response).status ~= 200 then
        return nil
    end
    return json.deserialize((response :: net.Response).body) :: any
end

-- The backend starts beside this: asked until it answers.
local balance: { [string]: any }? = nil
for _ = 1, 100 do
    balance = ask({ request = "probe", op = "balance", owner = Owner, currency = "Gold" })
    if balance then
        break
    end
    task.wait(0.1)
end
if balance == nil then
    print("[twice] the backend never answered")
    process.exit(1)
end

local pay = {
    request = "the-same-id",
    op = "transact",
    world = "town",
    moves = { { currency = "Gold", amount = 5, from = "", to = Owner } },
}
local first = ask(pay)
local second = ask(pay)
local once = (ask({ request = "after-two", op = "balance", owner = Owner, currency = "Gold" }) :: any).balance
pay.request = "another-id"
local third = ask(pay)
local twice = (ask({ request = "after-three", op = "balance", owner = Owner, currency = "Gold" }) :: any).balance

local same = first ~= nil and second ~= nil and json.serialize(first :: any) == json.serialize(second :: any)
if same and once == 5 and third ~= nil and twice == 10 then
    print(`[twice] once: the same id answered alike and moved {once}; a new id moved again to {twice}`)
    process.exit(0)
end
print(`[twice] FAILED: same={same} once={once} twice={twice}`)
process.exit(1)
