--!strict
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

task.spawn(function()
    while #NetworkService:GetPlayers() == 0 do
        RunService.PostSimulation:Wait()
    end
    local record = DataService:LoadAsync(NetworkService:GetPlayers()[1])
    record:Set("Gold", 30)
    record:Release()
    print("[migrate] wrote version 1")
end)
