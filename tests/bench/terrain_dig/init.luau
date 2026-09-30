--!strict
--- What digging costs where somebody stands (terrain audit P4, TA14).
---
--- `terrain_sculpt` has nothing that moves, so it never builds a collider: a
--- terrain collides only near bodies that move. This one is a character walking
--- a circle on the ground and digging ahead of itself every tick -- a player
--- with a shovel -- so every tick rebuilds the colliders round it, which is the
--- cost the sculpt bench cannot see.

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Rock = 1

local terrain = Instance.new("Terrain")
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain.Parent = Workspace
terrain:FillBlock(vector.create(0, -32, 0), vector.create(160, 64, 160), Rock)

local digger = Instance.new("CharacterBody")
digger.Name = "Digger"
digger.Size = vector.create(2, 5, 2)
digger.Position = vector.create(24, 3, 0)
digger.WalkSpeed = 8
digger.Parent = Workspace

local Ticks = 0
RunService.Heartbeat:Connect(function()
    Ticks += 1
    -- Round a circle of 24 m, so the dig moves through chunk after chunk.
    local angle = Ticks * 0.02
    local ahead = vector.create(-math.sin(angle), 0, math.cos(angle))
    digger:Move(ahead)
    local at = digger.Position + ahead * 3
    terrain:FillBall(vector.create(at.x, -1.5, at.z), 1.5, 0)

    -- A bench of a character standing on nothing measures nothing: late enough
    -- to have walked a while, it is still on the ground and has gone somewhere.
    if Ticks == 280 then
        assert(digger.Position.y > -8, `the digger fell through: {digger.Position.y}`)
        assert(vector.magnitude(digger.Position - vector.create(24, 3, 0)) > 4, "the digger did not move")
    end
end)
