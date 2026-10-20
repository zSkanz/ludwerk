--!strict
-- The fields, against the same Nakama: they count the shared value as the
-- town does, load the record the town carried here, and say what the game
-- holds once both worlds are done -- the run's last line.
local DataService = game:GetService("DataService")
local MatchmakingService = game:GetService("MatchmakingService")
local NetworkService = game:GetService("NetworkService")

if NetworkService.WorldName == "" then
    return
end

-- The queue Nakama's matchmaker fills, as every world of the game says it.
MatchmakingService:DefineQueue("Duel", { Template = "arena", Teams = 2, TeamSize = 1 })

local arrived: number? = nil

task.spawn(function()
    for _ = 1, 50 do
        DataService:UpdateAsync("nakama", "count", function(old: any): any
            return ((old :: number?) or 0) + 1
        end)
    end
    print("[nakama] fields counted")
end)

NetworkService.TravelArrived:Connect(function(player: Player)
    local record = DataService:LoadAsync(player)
    local level = (record:Get("Level") :: number?) or 0
    player:SetAttribute("Account", player.AccountId)
    player:SetAttribute("Level", level)
    arrived = level
end)

task.spawn(function()
    while true do
        local count = DataService:GetAsync("nakama", "count")
        local traded = DataService:GetAsync("nakama", "traded")
        local full = DataService:GetAsync("nakama", "arena")
        if count == 100 and traded == true and full == true and arrived ~= nil then
            local swords = #DataService:GetItemsAsync("ana") + #DataService:GetItemsAsync("bo")
            local gold = DataService:GetBalanceAsync("ana", "Gold") + DataService:GetBalanceAsync("bo", "Gold")
            print(`[nakama] done level={arrived} count={count} swords={swords} gold={gold}`)
            return
        end
        task.wait(0.5)
    end
end)
