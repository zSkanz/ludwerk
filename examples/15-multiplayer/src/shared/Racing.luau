--!strict
--[[
	What both sides of the game agree on (ADR 0105): how a racer is driven.

	A `ModuleScript` under `GlobalScriptService.Shared`, from `src/shared/`. The
	server drives every racer with it; a client that joined drives its own with
	it too, a tick ahead of the authority, and the snapshots correct it rather
	than overwrite it (ADR 0076). One function, so the two cannot disagree.
]]

local Racing = {}

--- Metres per second.
Racing.Speed = 8

--- Moves `racer` by what `player` pressed this tick.
function Racing.drive(player: Player, racer: BasePart, dt: number)
    local intent = player:GetIntent("Move")
    if typeof(intent) == "Vector2" then
        racer.Position += vector.create(intent.X, 0, -intent.Y) * Racing.Speed * dt
    end
end

return Racing
