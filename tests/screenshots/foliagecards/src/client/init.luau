--!strict

-- **A card's shadow has the card's holes** (terrain audit T3): a field of
-- upright cards on plain ground, the sun high, photographed from straight
-- above as the sun's shadow on the ground (`--debug-view=shadow`). The card's
-- material cuts it where its image's alpha is under a half; the driver runs it
-- with an image half cut away and with one whole, and the first must shadow
-- the ground by about half as much. The shadow pass read no alpha, and both
-- shadowed it the same.

local Lighting = game:GetService("Lighting")

local terrain = Instance.new("Terrain")
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({ "engine://terrain/grass" })
terrain.Parent = workspace
terrain:FillBlock(vector.create(0, -2, 0), vector.create(80, 4, 80), 1)

local field = Instance.new("FoliageLayer")
field.Name = "Cards"
field.Density = 1.5
field.MinSpacing = 0.6
field.DrawDistance = 200
field.FadeDistance = 1
field.Seed = 3
field:SetMaterials({ { Material = 1, Density = 1 } })
field.Parent = terrain

local card = Instance.new("FoliageMesh")
card.Name = "Card"
card.Mesh = "asset://models/card.gltf" :: Content
card.ScaleMin = 1
card.ScaleMax = 1
card.AlignToNormal = 0
card.WindResponse = 0
card.Parent = field

local camera = Instance.new("Camera")
camera.FieldOfView = 40
camera.CFrame = CFrame.lookAt(vector.create(0, 40, 0.01), vector.create(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.GeographicLatitude = 0
Lighting.ClockTime = 9
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
