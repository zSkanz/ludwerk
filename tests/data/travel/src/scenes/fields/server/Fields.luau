--!strict
-- The fields: the record of a player who arrives, as the town left it -- and
-- said on the player, for their own machine to read.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

NetworkService.TravelArrived:Connect(function(player: Player, _data: any, origin: string, _reason: string)
    local record = DataService:LoadAsync(player)
    local level = (record:Get("Level") :: number?) or 0
    player:SetAttribute("Level", level)
    print(`[travel] done: fields level={level} from={origin}`)
end)
