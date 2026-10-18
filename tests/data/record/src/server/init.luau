--!strict
-- A player's record on a server with no manager (ADR 0238 §2): loaded from
-- the server's own database, changed, and let go -- each run of this world
-- finds what the last one wrote.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

task.spawn(function()
    while #NetworkService:GetPlayers() == 0 do
        RunService.PostSimulation:Wait()
    end
    local player = NetworkService:GetPlayers()[1]
    local record = DataService:LoadAsync(player)
    local level = (record:Get("Level") :: number?) or 0
    record:Set("Level", level + 1)
    record:Set("Bag", { "sword", "shield" })
    local again = DataService:LoadAsync(player)
    local same = again == record
    local bag = record:Get("Bag") :: { string }
    record:Release()
    local refused = not pcall(function()
        record:Get("Level")
    end)
    print(`[data] level={level + 1} bag={#bag} same={same} released={record.Released} refused={refused}`)
end)
