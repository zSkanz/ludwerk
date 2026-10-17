--!strict
--[[
	A dungeon: an instance (ADR 0221), one process for one party. The town made
	it with `CreateInstanceAsync`; `SceneService:GetLoadData()` is what it
	handed over, and only the players it was made for may enter.

	The exit goes home with `Travel`. When the last of the party has left, the
	manager closes it a minute later (`idle_close_seconds`), `Closing` fires
	first, and the town hears `InstanceClosed`.
]]

local InstanceService = game:GetService("InstanceService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

local Realm = require("@shared/Realm")

local data = SceneService:GetLoadData()
if typeof(data) == "table" and typeof(data.Party) == "table" then
    print(`dungeon {InstanceService.InstanceId} opened for {table.concat(data.Party, ", ")}`)
end

local function home(player: Player)
    local ok = pcall(function()
        NetworkService:Travel(player, "town", { From = "dungeon" })
    end)
    if not ok then
        Realm.tell(player, "realm.notice.needs_servers")
    end
end

Realm.build({
    Name = "dungeon",
    Ground = Color3.fromRGB(58, 52, 70),
    Spawn = vector.create(0, 0, 12),
    Portals = {
        { Name = "Exit", Position = vector.create(0, 0, 16), Color = Color3.fromRGB(176, 160, 128), Enter = home },
    },
})

-- What the party came for, turning in the dark.
local treasure = Instance.new("Part")
treasure.Name = "Treasure"
treasure.Anchored = true
treasure.Size = vector.create(1.4, 1.4, 1.4)
treasure:SetMaterialParameter("Color", Color3.fromRGB(240, 200, 60))
treasure.Parent = workspace
RunService.Heartbeat:Connect(function()
    treasure.CFrame = CFrame.new(0, 1.5, -8) * CFrame.fromEuler(0, RunService.SimTime, 0.4)
end)

InstanceService.Closing:Connect(function()
    print(`dungeon {InstanceService.InstanceId} is closing`)
end)
