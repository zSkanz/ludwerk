--!strict
-- The dungeon, an instance made for Cyd alone: their arrival marked for their
-- machine, and said where the arena waits for it.
local NetworkService = game:GetService("NetworkService")

local EndToEnd = require("@shared/EndToEnd")

if NetworkService.WorldName == "" then
    return
end

EndToEnd.start()

NetworkService.TravelArrived:Connect(function(player: Player)
    player:SetAttribute("InDungeon", true)
    EndToEnd.set("dungeon", true)
    print(`[e2e] dungeon arrived={player.AccountId}`)
end)
