--!strict
--[[
	The joiner: what it was sent of each thing, against what the authority
	holds. One line a class a way a phase, which `run_carried.cmake` reads:

	    [carried] spawned Stamped.PointLight same <the line both ends read>
	    [carried] spawned Cloned.Trail DIFFERENT server=<...> client=<...>
	    [carried] changed Made.Sound MISSING

	`spawned` is as the things arrived; `changed` is after the authority wrote
	one property of each, and is `same` only when the copy here moved with it
	-- a line equal to the authority's and other than the one it had.
]]

local NetworkService = game:GetService("NetworkService")
local Workspace = game:GetService("Workspace")

local Carried = require("@shared/Carried")

if NetworkService.Authority then
    return
end

local held = Workspace:WaitForChild("Held", 20) :: RemoteFunction
local change = Workspace:WaitForChild("Change", 20) :: RemoteFunction

-- Read once the server's instances are here: until it is welcomed a machine
-- does not know the number it was given, and the things are named by it.
local function welcomed(): Player
    while true do
        local player = NetworkService.LocalPlayer
        if player ~= nil and player.UserId >= 2 then
            return player
        end
        task.wait()
    end
end
local number = welcomed().UserId
print(`[carried] joined as player {number}`)

-- This end's own line for a class under a way's thing, or nil.
local function mine(way: string, class: string): string?
    if way == "World" then
        return Carried.describeWorld(class)
    end
    local body = Workspace:FindFirstChild(`{way}{number}`)
    return if body then Carried.describe(body, class) else nil
end

-- Every line both ends speak of: a way and a class, and the world's own.
local keys: { { string } } = {}
for _, way in Carried.Ways do
    for _, class in Carried.Classes do
        table.insert(keys, { way, class })
    end
end
for _, name in Carried.World do
    table.insert(keys, { "World", name })
end

-- Waits, up to `seconds`, until this end reads what the authority reads of
-- everything: a snapshot is a few ticks behind the answer to a remote.
local function settle(theirs: { [string]: string }, seconds: number)
    local deadline = os.clock() + seconds
    while os.clock() < deadline do
        local all = true
        for _, key in keys do
            if mine(key[1], key[2]) ~= theirs[`{key[1]}.{key[2]}`] then
                all = false
            end
        end
        if all then
            return
        end
        task.wait(0.1)
    end
end

local function report(phase: string, theirs: { [string]: string }, before: { [string]: string }?): { [string]: string }
    local seen: { [string]: string } = {}
    for _, each in keys do
        local key = `{each[1]}.{each[2]}`
        local here = mine(each[1], each[2])
        local there = theirs[key]
        if here == nil or there == nil then
            print(`[carried] {phase} {key} MISSING`)
        elseif here ~= there then
            print(`[carried] {phase} {key} DIFFERENT server={there} client={here}`)
        elseif before ~= nil and before[key] == here then
            print(`[carried] {phase} {key} UNCHANGED {here}`)
        else
            print(`[carried] {phase} {key} same {here}`)
        end
        if here then
            seen[key] = here
        end
    end
    return seen
end

local first = held:InvokeServerAsync() :: { [string]: string }
settle(first, 15)
local spawned = report("spawned", first)

change:InvokeServerAsync()
local second = held:InvokeServerAsync() :: { [string]: string }
settle(second, 15)
report("changed", second, spawned)
print("[carried] done")
-- Nothing more to see: the run ends here and the server's with it, rather
-- than both sitting out the frames the driver allows for a slow machine.
game:Shutdown()
