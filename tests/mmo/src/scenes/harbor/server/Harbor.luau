--!strict
-- The harbor, the third world: Cyd arrives from the town, and once all three
-- have heard the guild's line, is sent to a dungeon of their own.
local InstanceService = game:GetService("InstanceService")
local MatchmakingService = game:GetService("MatchmakingService")
local NetworkService = game:GetService("NetworkService")

local EndToEnd = require("@shared/EndToEnd")

if NetworkService.WorldName == "" then
    return
end

MatchmakingService:DefineQueue("Duel", { Template = "arena", Teams = 2, TeamSize = 1 })
EndToEnd.start()

local cyd: Player? = nil
NetworkService.TravelArrived:Connect(function(player: Player, data: any)
    if typeof(data) == "table" and typeof(data.Name) == "string" then
        player.Name = data.Name
    end
    cyd = player
    EndToEnd.add("arrived")
    print(`[e2e] harbor arrived {player.Name}`)
end)

task.spawn(function()
    EndToEnd.waitFor(function()
        return cyd ~= nil and EndToEnd.count("heard") >= 3
    end)
    local party = { cyd :: Player }
    local made, id = pcall(function()
        return InstanceService:CreateInstanceAsync("dungeon", { Players = party })
    end)
    print(`[e2e] harbor dungeon made={made}`)
    if made then
        NetworkService:TravelGroup(party, id :: string, { From = "harbor" })
    end
end)
