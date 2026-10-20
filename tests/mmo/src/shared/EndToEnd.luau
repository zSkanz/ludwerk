--!strict
-- What every world of the proof has: the chat, a remote on which each player's
-- machine says its name and what it heard, and the shared values the worlds
-- go by -- one world cannot see another's players, so each step is a value in
-- the data service that the next one waits for.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local chat = require("@engine/chat")

local EndToEnd = {}

--- The line the founder says in the guild's channel, and each member hears.
EndToEnd.GuildLine = "for the wardens"

--- One more of `key`, from whichever world.
function EndToEnd.add(key: string)
    DataService:UpdateAsync("e2e", key, function(old: any): any
        return ((old :: number?) or 0) + 1
    end)
end

function EndToEnd.count(key: string): number
    return (DataService:GetAsync("e2e", key) :: number?) or 0
end

function EndToEnd.set(key: string, value: any)
    local _, version = DataService:GetAsync("e2e", key)
    DataService:SetAsync("e2e", key, value, version)
end

function EndToEnd.get(key: string): any
    return (DataService:GetAsync("e2e", key))
end

--- Waits until `ready()` holds, half a second at a time.
function EndToEnd.waitFor(ready: () -> boolean)
    while not ready() do
        task.wait(0.5)
    end
end

--- Started by every world's server: the chat, and the remote. `named` is
--- called when a player's machine says its name; a machine that says it
--- heard the guild's line counts once.
function EndToEnd.start(named: ((player: Player, name: string) -> ())?)
    chat.start()
    local remote = Instance.new("RemoteEvent")
    remote.Name = "E2E"
    remote.Parent = ReplicatedStorage
    local heard: { [string]: boolean } = {}
    remote.ServerReceived:Connect(function(player: Player, what: any, value: any)
        if what == "name" and typeof(value) == "string" then
            player.Name = value
            if named then
                named(player, value)
            end
        elseif what == "heard" and player.AccountId ~= nil and not heard[player.AccountId :: string] then
            heard[player.AccountId :: string] = true
            EndToEnd.add("heard")
            print(`[e2e] {NetworkService.WorldName} heard by {player.Name}`)
        end
    end)
end

return EndToEnd
