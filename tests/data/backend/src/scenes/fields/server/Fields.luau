--!strict
-- The fields, against the same backend: they count the shared value as the
-- town does, load the record the town carried here, and say what the game
-- holds once both worlds are done -- the run's last line.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

if NetworkService.WorldName == "" then
    return
end

local arrived: number? = nil

task.spawn(function()
    for _ = 1, 50 do
        DataService:UpdateAsync("backend", "count", function(old: any): any
            return ((old :: number?) or 0) + 1
        end)
    end
    print("[backend] fields counted")
end)

NetworkService.TravelArrived:Connect(function(player: Player)
    local record = DataService:LoadAsync(player)
    local level = (record:Get("Level") :: number?) or 0
    player:SetAttribute("Level", level)
    arrived = level
end)

task.spawn(function()
    while true do
        local count = DataService:GetAsync("backend", "count")
        local traded = DataService:GetAsync("backend", "traded")
        if count == 100 and traded == true and arrived ~= nil then
            local swords = #DataService:GetItemsAsync("ana") + #DataService:GetItemsAsync("bo")
            local gold = DataService:GetBalanceAsync("ana", "Gold") + DataService:GetBalanceAsync("bo", "Gold")
            print(`[backend] done level={arrived} count={count} swords={swords} gold={gold}`)
            return
        end
        task.wait(0.5)
    end
end)
