--!strict
-- The town, against the game's Nakama: a thousand trades of a sword for
-- its price between two owners, a shared value counted from here and from
-- the fields at once, and a player's record changed and carried away.
local DataService = game:GetService("DataService")
local MatchmakingService = game:GetService("MatchmakingService")
local NetworkService = game:GetService("NetworkService")

-- The player's own machine, before it joins, keeps nothing.
if NetworkService.WorldName == "" then
    return
end

-- The queue Nakama's matchmaker fills, as every world of the game says it.
MatchmakingService:DefineQueue("Duel", { Template = "arena", Teams = 2, TeamSize = 1 })

local Owners = { "ana", "bo" }
local Trades = 1000

task.spawn(function()
    if #DataService:GetItemsAsync("ana") + #DataService:GetItemsAsync("bo") == 0 then
        for _ = 1, 5 do
            DataService:CreateItemAsync("ana", "Sword", { Edge = 3 })
        end
        DataService:TransactAsync({ { Pay = 1000, Currency = "Gold", To = "bo" } })
    end
    -- A sword one way and its price the other, whichever way can.
    local traded = 0
    while traded < Trades do
        for index, seller in Owners do
            local buyer = Owners[3 - index]
            local items = DataService:GetItemsAsync(seller)
            if traded < Trades and #items > 0 then
                local ok = DataService:TransactAsync({
                    { Move = items[1].Id, From = seller, To = buyer },
                    { Pay = 10, Currency = "Gold", From = buyer, To = seller },
                })
                if ok then
                    traded += 1
                end
            end
        end
    end
    local _, version = DataService:GetAsync("nakama", "traded")
    DataService:SetAsync("nakama", "traded", true, version)
    print(`[nakama] town traded={traded}`)
end)

task.spawn(function()
    for _ = 1, 50 do
        DataService:UpdateAsync("nakama", "count", function(old: any): any
            return ((old :: number?) or 0) + 1
        end)
    end
    print("[nakama] town counted")
end)

NetworkService.PlayerAdded:Connect(function(player: Player)
    local record = DataService:LoadAsync(player)
    local level = ((record:Get("Level") :: number?) or 0) + 7
    record:Set("Level", level)
    NetworkService:Travel(player, "fields", {})
    print(`[nakama] town set level={level}`)
end)
