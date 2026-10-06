--!strict

-- **A part drawn in an instanced run is lit as the same part drawn alone**
-- (D577). Six cubes, each turned an eighth of a turn about the vertical so the
-- camera sees two faces of it, with the sun low in the east: the face turned
-- to the sun is the bright one. Five wear one material and are drawn as one
-- instanced run; the sixth wears another and is a draw of its own. The run's
-- shader built its normal matrix from the cofactor's columns as ROWS -- the
-- transpose, which for a rotation is the inverse -- so a turned part's
-- normals were turned the other way, and on these cubes the face in the sun
-- was the dark one.
--
-- **The sixth's material differs in roughness, not in colour**: a run takes
-- each instance's colour with it, so a cube of another colour alone would
-- have joined the five.

local Lighting = game:GetService("Lighting")

local function material(name: string): Material
    return Material.load(`asset://materials/{name}.material.json` :: any)
end

local function cube(x: number, wearing: Material)
    local made = Instance.new("Part")
    made.Anchored = true
    -- No shadows: the cubes stand in a row along the sun's own direction, and
    -- one in its neighbour's shadow would be dark for another reason.
    made.CastShadow = false
    made.Size = vector.create(1.6, 1.6, 1.6)
    made.CFrame = CFrame.new(x, 1, 0) * CFrame.fromEuler(0, math.pi / 4, 0)
    made.Material = wearing
    made.Parent = workspace
end

local run = material("run")
for _, x in { -7.5, -4.5, -1.5, 4.5, 7.5 } do
    cube(x, run)
end
cube(1.5, material("alone"))

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(0, 1, 16), vector.create(0, 1, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The sun low in the east, and little else: what tells a cube's two faces
-- apart is the sun's light alone. The exposure held still, so the picture is
-- the same one every run.
Lighting.GeographicLatitude = 0
Lighting.ClockTime = 8
Lighting.Brightness = 3
Lighting.Ambient = Color3.fromRGB(20, 20, 20)
Lighting.OutdoorAmbient = Color3.fromRGB(20, 20, 20)
Lighting.EnvironmentDiffuseScale = 0.1
Lighting.EnvironmentSpecularScale = 0
Lighting.AutoExposure = false
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
