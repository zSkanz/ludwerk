--!strict
-- The fields: a traveller is the same account here, by the pass that brought
-- them, and their record is the one the town left.
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")

NetworkService.TravelArrived:Connect(function(player: Player, _data: any, origin: string, _reason: string)
    local record = DataService:LoadAsync(player)
    local level = (record:Get("Level") :: number?) or 0
    player:SetAttribute("Account", player.AccountId)
    player:SetAttribute("Level", level)
    print(`[accounts] done: fields account={player.AccountId} level={level} from={origin}`)
end)
