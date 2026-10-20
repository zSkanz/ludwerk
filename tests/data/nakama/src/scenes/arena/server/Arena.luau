--!strict
-- The arena: the instance of a match Nakama's matchmaker made. Each player who
-- arrives is said, and marked for their own machine to see.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

if NetworkService.WorldName == "" then
    return
end

local arrived = 0
NetworkService.TravelArrived:Connect(function(player: Player)
    arrived += 1
    player:SetAttribute("InArena", true)
    print(`[nakama] arena arrived={player.AccountId}`)
    if arrived == 2 then
        -- Said where the fields read it: the run's last line waits for it.
        local _, version = DataService:GetAsync("nakama", "arena")
        DataService:SetAsync("nakama", "arena", true, version)
        print("[nakama] arena full")
    end
end)
