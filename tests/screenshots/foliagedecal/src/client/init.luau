--!strict
--[[
	A decal paints the foliage that stands in it (ADR 0185).

	Two fields of tall upright cards, side by side, on plain ground: the left
	one receives decals, as a layer does unless it says otherwise, and the
	right one has `ReceivesDecals` off. A red mark is laid down over the middle
	of both at tick 20, pointing straight down -- at a card edge-on, which is
	what a blade of grass is to a mark on the ground under it.

	`run_foliage_decal_gate.cmake` takes a picture before the mark and one
	after, and asks that the left field's cards inside the mark are darker than
	its cards outside it, and that the right field's are the same pixels in
	both.

	An orthographic camera twenty metres wide over a 1000 by 500 window,
	looking along the fields from in front and a little above: a thing's middle
	is at x = (metres + 10) / 20.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.GeographicLatitude = 0
Lighting.ClockTime = 12
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

local terrain = Instance.new("Terrain")
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({ "engine://terrain/rock", "engine://terrain/sand" })
-- Each field its own ground, so each layer grows on its own side: rock under
-- the left field and sand under the right.
terrain:FillBlock(vector.create(-30, -2, 0), vector.create(60, 4, 120), 1)
terrain:FillBlock(vector.create(30, -2, 0), vector.create(60, 4, 120), 2)
terrain.Parent = workspace

local function field(name: string, material: number, receives: boolean)
    local layer = Instance.new("FoliageLayer")
    layer.Name = name
    layer.Density = 3
    layer.MinSpacing = 0.3
    layer.DrawDistance = 400
    layer.FadeDistance = 1
    layer.Seed = 7
    layer.ReceivesDecals = receives
    layer:SetMaterials({ { Material = material, Density = 1 } })
    layer.Parent = terrain

    local card = Instance.new("FoliageMesh")
    card.Name = "Card"
    card.Mesh = "asset://models/card.gltf" :: Content
    card.ScaleMin = 4
    card.ScaleMax = 4
    card.RandomRotation = false
    card.AlignToNormal = 0
    card.WindResponse = 0
    card.CastShadow = false
    card.Parent = layer
end

field("Receiving", 1, true)
field("Refusing", 2, false)

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.lookAt(vector.create(0, 14, 30), vector.create(0, 2, 0))

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
    if ticks == 20 then
        local decal = Instance.new("Decal")
        -- Looking down, eight metres either side of the line between the
        -- fields, the fields' whole depth, and from under the ground to over
        -- the cards' tops.
        decal.CFrame = CFrame.lookAt(vector.create(0, 4, -15), vector.create(0, 5, -15))
        decal.Size = vector.create(16, 60, 12)
        decal.Color = Color3.fromRGB(255, 40, 30)
        decal.Parent = workspace
    end
end)
