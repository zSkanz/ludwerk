--!strict
--[[
	What both ends say of a part, in one line the driver reads: its mesh's
	measure, its size, and what a ray dropped just inside and just outside
	its half-width finds -- the collider is its `Size`.
]]

local Workspace = game:GetService("Workspace")

local Measured = {}

-- The pane the content holds: a cube scaled to 2.8 by 2 by 0.08.
Measured.Mesh = "asset://models/pane.gltf"
-- The parts the server makes, each a way of meeting a mesh.
Measured.Parts = { "Served", "Sized", "Missing" }

local function vec(value: vector): string
    return string.format("%.4f,%.4f,%.4f", value.x, value.y, value.z)
end

local function drop(part: BasePart, along: number): string
    local from = part.Position + vector.create(along, 20, 0)
    local hit = Workspace:Raycast(from, vector.create(0, -40, 0))
    return if hit then hit.Instance.Name else "miss"
end

function Measured.describe(part: MeshPart): string
    local half = part.Size.x / 2
    return `MeshSize={vec(part.MeshSize)} Size={vec(part.Size)} `
        .. `Inside={drop(part, half - 0.05)} Outside={drop(part, half + 0.05)}`
end

return Measured
