--!strict
--[[
	How a player's character moves, for both sides: a `ModuleScript` under
	`GlobalScriptService.Shared`, from `src/shared/`.

	Each world's server moves every character with it. A client moves its own
	with it too, at once, from its own keys, and the server's snapshots correct
	it -- one function, so the two cannot disagree about where a key takes you.
]]

local Walk = {}

--- Metres per second.
Walk.Speed = 10

--- How far from a world's centre a character may go, on each axis: every
--- world's ground is forty metres across.
Walk.Bound = 19

--- The height a character's centre stands at.
Walk.Height = 0.8

--- Moves `character` by what `player` pressed this tick.
function Walk.step(player: Player, character: BasePart, dt: number)
    local intent = player:GetIntent("Move")
    if typeof(intent) ~= "Vector2" then
        return
    end
    local at = character.Position + vector.create(intent.X, 0, -intent.Y) * Walk.Speed * dt
    character.Position =
        vector.create(math.clamp(at.x, -Walk.Bound, Walk.Bound), Walk.Height, math.clamp(at.z, -Walk.Bound, Walk.Bound))
end

return Walk
