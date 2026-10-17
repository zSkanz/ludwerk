--!strict
--[[
	The fields: another world, another process, the same game. A player who
	came by the town's road arrives with what the town handed them
	(`TravelArrived`'s `data`), and the road back is one `Travel`.
]]

local NetworkService = game:GetService("NetworkService")

local Realm = require("@shared/Realm")

local function toTown(player: Player)
    local ok = pcall(function()
        NetworkService:Travel(player, "town")
    end)
    if not ok then
        Realm.tell(player, "realm.notice.needs_servers")
    end
end

Realm.build({
    Name = "fields",
    Ground = Color3.fromRGB(92, 150, 72),
    Spawn = vector.create(-10, 0, -6),
    Portals = {
        { Name = "Road", Position = vector.create(-10, 0, -14), Color = Color3.fromRGB(176, 160, 128), Enter = toTown },
    },
})

NetworkService.TravelArrived:Connect(function(player: Player, data: any, origin: string)
    if typeof(data) == "table" and typeof(data.Gift) == "string" then
        print(`{player.Name} came from the {origin} with {data.Gift}`)
    end
    Realm.tell(player, "realm.notice.arrived_fields")
end)
