--!strict
-- Each run first counts what the last one left -- five swords between two
-- owners, and a thousand gold -- then trades until it is killed.
local DataService = game:GetService("DataService")
local RunService = game:GetService("RunService")

local Owners = { "ana", "bo" }

task.spawn(function()
    local swords = 0
    local gold = 0
    for _, owner in Owners do
        swords += #DataService:GetItemsAsync(owner)
        gold += DataService:GetBalanceAsync(owner, "Gold")
    end
    if swords == 0 and gold == 0 then
        for _ = 1, 5 do
            DataService:CreateItemAsync("ana", "Sword", { Edge = 3 })
        end
        DataService:TransactAsync({ { Pay = 1000, Currency = "Gold", To = "bo" } })
        swords, gold = 5, 1000
    end
    print(`[trade] swords={swords} gold={gold}`)
    -- A sword one way and its price the other, whichever way can.
    local traded = 0
    local said = 0
    while true do
        for index, seller in Owners do
            local buyer = Owners[3 - index]
            local items = DataService:GetItemsAsync(seller)
            if #items > 0 then
                local ok = DataService:TransactAsync({
                    { Move = items[1].Id, From = seller, To = buyer },
                    { Pay = 10, Currency = "Gold", From = buyer, To = seller },
                })
                if ok then
                    traded += 1
                end
            end
        end
        if traded >= said + 50 then
            said = traded
            print(`[trade] traded={traded}`)
        end
        RunService.PostSimulation:Wait()
    end
end)
