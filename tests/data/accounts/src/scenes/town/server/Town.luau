--!strict
-- The town: a player who arrives is known by the account the front door
-- signed in, with no `OnJoinRequest` of the game's; their record is changed
-- and they are sent to the fields in the same tick.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

-- Ends the run if nobody comes: the test's manager stops at a `[accounts] done`.
task.delay(90, function()
    print("[accounts] done: nobody arrived")
end)

NetworkService.PlayerAdded:Connect(function(player: Player)
    -- The player's own machine, before it joins, has no records to load.
    local loaded, record = pcall(function()
        return DataService:LoadAsync(player)
    end)
    if not loaded then
        return
    end
    local level = ((record:Get("Level") :: number?) or 0) + 7
    record:Set("Level", level)
    NetworkService:Travel(player, "fields", {})
    print(`[accounts] town account={player.AccountId} level={level}`)
end)
