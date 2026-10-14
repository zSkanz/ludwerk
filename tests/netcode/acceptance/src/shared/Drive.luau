--!strict
--[[
	How a character is driven, the same on both sides (ADR 0105): the server
	drives every player's with it, and the client drives its own with it a
	tick ahead, which is what is corrected when the two disagree.
]]

local Drive = {}

--- Walks `body` the way `player`'s `Move` intent says, this tick.
function Drive.step(player: Player, body: CharacterBody)
    local intent = player:GetIntent("Move")
    if typeof(intent) == "Vector2" then
        body:Move(vector.create(intent.X, 0, -intent.Y))
    else
        body:Move(vector.zero)
    end
end

return Drive
