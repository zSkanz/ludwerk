--!strict
-- The arena, the instance of the duel the manager matched: each player who
-- arrives is marked for their machine. With both here and Cyd in the
-- dungeon, the two trade -- five swords and a thousand gold between them, a
-- sword one way and its price the other -- until the manager is ended in
-- the middle of a trade. The traders are kept for the next run to count.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local EndToEnd = require("@shared/EndToEnd")

if NetworkService.WorldName == "" then
    return
end

EndToEnd.start()

local function trade(owners: { string })
    EndToEnd.waitFor(function()
        return EndToEnd.get("dungeon") == true
    end)
    -- Every machine has seen where it is before the manager is ended.
    task.wait(2)
    EndToEnd.set("traders", owners)
    for _ = 1, 5 do
        DataService:CreateItemAsync(owners[1], "Sword", { Edge = 3 })
    end
    DataService:TransactAsync({ { Pay = 1000, Currency = "Gold", To = owners[2] } })
    print("[e2e] arena trading swords=5 gold=1000")
    local traded = 0
    while true do
        for index, seller in owners do
            local buyer = owners[3 - index]
            local items = DataService:GetItemsAsync(seller)
            if #items > 0 then
                local ok = DataService:TransactAsync({
                    { Move = items[1].Id, From = seller, To = buyer },
                    { Pay = 10, Currency = "Gold", From = buyer, To = seller },
                })
                if ok then
                    traded += 1
                    if traded % 5 == 0 then
                        print(`[e2e] arena traded={traded}`)
                    end
                end
            end
        end
        RunService.PostSimulation:Wait()
    end
end

local arrived: { Player } = {}
NetworkService.TravelArrived:Connect(function(player: Player)
    table.insert(arrived, player)
    player:SetAttribute("InArena", true)
    print(`[e2e] arena arrived={player.AccountId}`)
    local first, second = arrived[1].AccountId, if arrived[2] then arrived[2].AccountId else nil
    if #arrived == 2 and first ~= nil and second ~= nil then
        task.spawn(trade, { first, second })
    end
end)
