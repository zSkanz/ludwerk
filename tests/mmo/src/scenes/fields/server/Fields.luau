--!strict
-- The fields: Bob arrives from the town, and once all three have heard the
-- guild's line, queues for a duel from here -- a world apart from Ana's.
local MatchmakingService = game:GetService("MatchmakingService")
local NetworkService = game:GetService("NetworkService")

local EndToEnd = require("@shared/EndToEnd")

if NetworkService.WorldName == "" then
    return
end

MatchmakingService:DefineQueue("Duel", { Template = "arena", Teams = 2, TeamSize = 1 })
EndToEnd.start()

local bob: Player? = nil
NetworkService.TravelArrived:Connect(function(player: Player, data: any)
    if typeof(data) == "table" and typeof(data.Name) == "string" then
        player.Name = data.Name
    end
    bob = player
    EndToEnd.add("arrived")
    print(`[e2e] fields arrived {player.Name}`)
end)

task.spawn(function()
    EndToEnd.waitFor(function()
        return bob ~= nil and EndToEnd.count("heard") >= 3
    end)
    local ticket, refused = MatchmakingService:QueueAsync(bob :: Player, "Duel")
    print(`[e2e] fields queued bob={ticket ~= nil} {refused or ""}`)
end)
