--!strict
--[[
	Mirrors and portals (ADR 0107), with `@engine/views`.

	**A mirror is one call**: `views.mirror(part)` puts the picture on the
	part's front face and moves a camera behind the glass every tick, reflected
	from wherever the world's camera is. Walk past it and the room in it moves
	the way a room in a mirror does.

	**A portal is one call too**: `views.portal(a, b)` shows, in `a`, what is in
	front of `b` -- here a garden room across the map, seen through a door in
	the gallery. A pair both ways is two calls.

	Both keep what stands behind the glass out of the picture with the
	camera's `ClipPlane`, and both are exact: the camera looks straight through
	the glass and the glass is cut out of its picture, so the reflection lines
	up with the room from any angle.
]]

local RunService = game:GetService("RunService")
local views = require("@engine/views")

local function block(position: vector, size: vector, color: Color3, facing: vector?): Part
    local part = Instance.new("Part")
    part.Anchored = true
    part.Size = size
    part.CFrame = if facing
        then CFrame.lookAt(position, position + facing)
        else CFrame.new(position.x, position.y, position.z)
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

-- --- The gallery ----------------------------------------------------------

block(vector.create(0, -0.5, 0), vector.create(30, 1, 30), Color3.fromRGB(120, 110, 100))
block(vector.create(0, 4, -12), vector.create(30, 8, 0.5), Color3.fromRGB(200, 196, 188))
block(vector.create(-15, 4, 0), vector.create(0.5, 8, 30), Color3.fromRGB(190, 186, 178))
-- Things to see in the mirror: a statue, a plant, a red chair.
block(vector.create(-4, 1.5, -4), vector.create(1.2, 3, 1.2), Color3.fromRGB(235, 235, 235))
block(vector.create(-8, 1, -2), vector.create(1.5, 2, 1.5), Color3.fromRGB(70, 160, 80))
block(vector.create(-1, 0.6, -6), vector.create(1.4, 1.2, 1.4), Color3.fromRGB(200, 50, 45))

-- The mirror: a frame on the back wall and the glass inside it, facing the room.
block(vector.create(-5, 3.2, -11.6), vector.create(6.4, 4.4, 0.2), Color3.fromRGB(60, 45, 30))
local glass =
    block(vector.create(-5, 3.2, -11.45), vector.create(6, 4, 0.1), Color3.fromRGB(40, 40, 45), vector.create(0, 0, 1))
views.mirror(glass, { Resolution = 768 })

-- The door: a portal in the back wall onto the garden room.
block(vector.create(6, 2.5, -11.6), vector.create(4, 5.4, 0.2), Color3.fromRGB(60, 45, 30))
local door = block(
    vector.create(6, 2.4, -11.45),
    vector.create(3.4, 4.8, 0.1),
    Color3.fromRGB(20, 20, 24),
    vector.create(0, 0, 1)
)

-- --- The garden room, far away ----------------------------------------------

local garden = vector.create(200, 0, 0)
block(garden + vector.create(0, -0.5, 0), vector.create(24, 1, 24), Color3.fromRGB(80, 140, 70))
for index = 0, 5 do
    local angle = index * math.pi / 3
    block(
        garden + vector.create(math.cos(angle) * 6, 1.5, math.sin(angle) * 6),
        vector.create(1, 3, 1),
        Color3.fromHSV(index / 6, 0.7, 0.9)
    )
end
-- The garden's side of the door: the portal looks out of this pane.
local gardenDoor = block(
    garden + vector.create(0, 2.4, 11),
    vector.create(3.4, 4.8, 0.1),
    Color3.fromRGB(20, 20, 24),
    vector.create(0, 0, -1)
)
views.portal(door, gardenDoor, { Resolution = 768 })

-- --- A camera walking past --------------------------------------------------

local camera = Instance.new("Camera")
camera.FieldOfView = 65
camera.Parent = workspace
workspace.CurrentCamera = camera

local Clock = 0
RunService.Heartbeat:Connect(function(dt: number)
    Clock += dt
    local x = math.sin(Clock * 0.25) * 8
    local eye = vector.create(x, 2.6, 6)
    camera.CFrame = CFrame.lookAt(eye, vector.create(x * 0.3, 2.8, -11))
end)
