--!strict
-- The town: each player's record is loaded, changed, and the player sent to
-- the fields in the same tick -- nothing written before the hop but what the
-- hop itself writes.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

-- Ends the run if nobody comes: the test's manager stops at a `[travel] done`.
task.delay(60, function()
    print("[travel] done: nobody arrived")
end)

NetworkService.PlayerAdded:Connect(function(player: Player)
    -- The player's own machine, alone before it joins, keeps no records.
    local loaded, record = pcall(function()
        return DataService:LoadAsync(player)
    end)
    if not loaded then
        return
    end
    local level = ((record:Get("Level") :: number?) or 0) + 7
    record:Set("Level", level)
    NetworkService:Travel(player, "fields", {})
    print(`[travel] town set level={level}`)
end)
