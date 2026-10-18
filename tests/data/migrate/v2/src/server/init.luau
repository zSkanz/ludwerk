--!strict
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local migrated = 0
DataService.Version = 2
DataService.OnMigrate = function(record: DataRecord, fromVersion: number)
    migrated += 1
    if fromVersion < 2 then
        record:Set("Purse", { Gold = record:Get("Gold") })
        record:Remove("Gold")
    end
end

task.spawn(function()
    while #NetworkService:GetPlayers() == 0 do
        RunService.PostSimulation:Wait()
    end
    local record = DataService:LoadAsync(NetworkService:GetPlayers()[1])
    local purse = record:Get("Purse") :: { Gold: number }?
    print(`[migrate] migrated={migrated} gold={purse and purse.Gold} old={record:Get("Gold")}`)
    record:Release()
end)
