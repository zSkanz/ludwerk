--!strict

-- **A terrain layer reads only the channels its maps promise** (terrain audit
-- TA13): photographed twice by `run_terrain_channels.cmake`, once wearing a
-- material whose metallic-roughness map has nothing in R -- as glTF allows --
-- and once wearing the same material with no map at all. The map's G and B
-- say what the factors say, so the two pictures must be the same one. The
-- terrain read R as occlusion, and the first drew its ambient at four tenths.
--
-- The driver copies the project and puts one of `variants/` in as
-- `content/materials/ground.material.json` before each run.

local Lighting = game:GetService("Lighting")

local terrain = Instance.new("Terrain")
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({ "asset://materials/ground.material.json" })
terrain.Parent = workspace
terrain:FillBlock(vector.create(0, -2, 0), vector.create(96, 4, 96), 1)
terrain:FillBall(vector.create(0, 2, 0), 6, 1)

local camera = Instance.new("Camera")
camera.FieldOfView = 55
camera.CFrame = CFrame.lookAt(vector.create(18, 10, 18), vector.create(0, 2, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 10
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
