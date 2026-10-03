--!strict

-- **Every part of an instanced run drawn where it is** (D484). The draws come
-- in material order: a run of grey cubes, instanced; a blue ball and a green
-- cube, one draw each, on two other meshes; then the red row, instanced, on
-- the cube mesh and the instance stream the grey run bound. On D3D12 the
-- vertex buffer views kept the stride the last single draw's pipeline gave the
-- instance stream -- zero -- and every red cube was drawn where the first is.

local Lighting = game:GetService("Lighting")

local function material(name: string): Material
    return Material.load(`asset://materials/{name}.material.json` :: any)
end

local function part(at: vector, wearing: Material, ball: boolean?): Part
    local made = Instance.new("Part")
    made.Anchored = true
    made.Size = vector.create(1.6, 1.6, 1.6)
    if ball then
        made.Shape = Enum.PartShape.Ball
    end
    made.Position = at
    made.Material = wearing
    made.Parent = workspace
    return made
end

-- Made in the order their materials are to be drawn in.
local grey = material("grey")
for at = 0, 7 do
    part(vector.create(at * 2 - 7, 4, -6), grey)
end
part(vector.create(-9, 1, 0), material("blue"), true)
part(vector.create(9, 1, 0), material("green"))
local red = material("red")
for at = 0, 7 do
    part(vector.create(at * 2 - 7, 1, 0), red)
end

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(0, 3, 16), vector.create(0, 2, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 14
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
