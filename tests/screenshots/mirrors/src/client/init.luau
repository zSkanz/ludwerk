--!strict
--[[
	The probe scene for mirrors and portals (ADR 0107, `@engine/views`).

	The world's camera looks down -Z at two panes six metres away: a mirror on
	the left and a portal on the right. Rendered at 1000 by 500.

	- **The mirror** shows a red and a blue block standing BEHIND the camera,
	  red on the left, as a mirror would. A green block stands behind the glass,
	  between the glass and the reflected camera: without the mirror's clip
	  plane it would fill the picture, and it must not be in it.
	- **The portal** looks out of a second pane a hundred metres away, facing
	  +X, with a yellow block in front of it and a purple one behind it: yellow
	  shows, purple does not.
]]

local views = require("@engine/views")

local function block(position: vector, size: vector, color: Color3, facing: vector?): Part
    local part = Instance.new("Part")
    part.Anchored = true
    part.Size = size
    local at = position
    part.CFrame = if facing then CFrame.lookAt(at, at + facing) else CFrame.new(at.x, at.y, at.z)
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 2, 0), vector.create(0, 2, -10))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The mirror, facing the camera, and what it should and should not show.
local mirror =
    block(vector.create(-3, 2, -6), vector.create(5, 4, 0.2), Color3.new(0.2, 0.2, 0.2), vector.create(0, 0, 1))
block(vector.create(-6, 2, 4), vector.create(2, 3, 1), Color3.new(1, 0, 0))
block(vector.create(-3, 2, 4), vector.create(2, 3, 1), Color3.new(0, 0, 1))
block(vector.create(-3, 2, -8), vector.create(4, 4, 0.5), Color3.new(0, 1, 0))
views.mirror(mirror, { ViewName = "mirror" })

-- The portal: in through the near pane, out of the far one.
local near =
    block(vector.create(3.5, 2, -6), vector.create(4, 4, 0.2), Color3.new(0.2, 0.2, 0.2), vector.create(0, 0, 1))
local far = block(vector.create(100, 2, 0), vector.create(4, 4, 0.2), Color3.new(0.2, 0.2, 0.2), vector.create(1, 0, 0))
block(vector.create(106, 2, 0), vector.create(1, 6, 6), Color3.new(1, 1, 0))
block(vector.create(98, 2, 0), vector.create(0.5, 6, 6), Color3.new(0.6, 0, 1))
views.portal(near, far, { ViewName = "portal" })
