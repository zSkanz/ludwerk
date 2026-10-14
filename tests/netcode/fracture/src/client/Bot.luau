--!strict
--[[
	The bot: once joined, it counts the bytes it receives in a quiet second,
	asks the server to break the wall, counts them over the three seconds the
	pieces fall and the two after, and how many pieces arrived. One line,
	`[fracture] ...`, which the gate's driver reads.
]]

local NetworkService = game:GetService("NetworkService")
local Workspace = game:GetService("Workspace")

if NetworkService.Authority then
    return
end

local function received(seconds: number): number
    local before = NetworkService:GetStats().BytesReceived
    task.wait(seconds)
    return (NetworkService:GetStats().BytesReceived - before) / seconds
end

task.spawn(function()
    while NetworkService.State ~= Enum.NetworkState.Connected do
        task.wait()
    end
    task.wait(2)
    local quiet = received(1)
    local cut = Workspace:WaitForChild("Cut") :: RemoteFunction
    local made = cut:InvokeServerAsync() :: number
    -- Cut and standing, nothing touching it: what a wall that is breakable
    -- costs before anything happens to it.
    task.wait(2)
    local standing = received(2)
    local strike = Workspace:WaitForChild("Strike") :: RemoteFunction
    strike:InvokeServerAsync()
    local falling = received(3)
    local settled = received(3)
    local arrived = 0
    local model = Workspace:FindFirstChild("Wall")
    if model then
        for _, child in model:GetChildren() do
            if child:IsA("BasePart") and child.Name ~= "Footing" then
                arrived += 1
            end
        end
    end
    print(
        `[fracture] made={made} arrived={arrived} quiet={math.floor(quiet)} standing={math.floor(standing)} `
            .. `falling={math.floor(falling)} `
            .. `settled={math.floor(settled)}`
    )
end)
