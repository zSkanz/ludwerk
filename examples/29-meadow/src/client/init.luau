--!strict
--[[
	A meadow (ADR 0116): rolling ground, grass and flowers over all of it, and a
	wind that moves them.

	**Nothing here places a blade.** A `FoliageLayer` under the terrain says
	where things grow -- by the ground's slope, height and material -- and its
	`FoliageMesh` children say what; the engine grows each tile of ground as
	the camera comes near it, from the same rules and seed on every machine,
	and draws the field with a cull on the GPU. The grass on the steep bank
	thins out because of `SlopeMax`, and none grows in the hollow under the
	arch because the ground there cannot see the sky.
]]

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local GrassContent: Content = "asset://models/grass.gltf"
local FlowerContent: Content = "asset://models/flower.gltf"

--- A material id: the terrain's first layer is grass.
local Grass = 1

--- Seconds for the camera's lap of the meadow.
local LapSeconds = 60

local terrain = Instance.new("Terrain")
-- **The ground's materials are this project's own files**, in
-- `content/materials/terrain/`: a new terrain comes with none. Each file is a
-- variant of one of the engine's built-in terrain materials, so it looks like
-- that one until somebody opens it in Content and changes it. The order is
-- the material ids the script paints with -- 1 grass, 2 sand, 3 rock.
terrain:SetLayers({
    "asset://materials/terrain/grass.material.json",
    "asset://materials/terrain/sand.material.json",
    "asset://materials/terrain/rock.material.json",
    "asset://materials/terrain/snow.material.json",
    "asset://materials/terrain/mud.material.json",
    "asset://materials/terrain/sandstone.material.json",
    "asset://materials/terrain/basalt.material.json",
    "asset://materials/terrain/ice.material.json",
})
-- Rock on anything steeper than about 45 degrees, drawn over every layer but
-- rock and basalt -- what a terrain used to start with.
terrain:SetRules({
    { Material = 3, SlopeMin = 45.35, Blend = 9.7, Noise = 0.12, AppliesTo = { 1, 2, 4, 5, 6, 8 } },
})
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain.Parent = Workspace

-- The meadow: a plain reaching down to the floor, then hills on it.
terrain:FillBlock(vector.create(0, -40, 0), vector.create(256, 80, 256), Grass)
-- Low caps of big balls, so their sides are pasture rather than cliffs: a
-- ball of radius 26 whose top stands 4 m up meets the plain at about 32°.
local hills: { { centre: vector, radius: number } } = {
    { centre = vector.create(30, -16, 20), radius = 20 },
    { centre = vector.create(-40, -22, -10), radius = 26 },
    { centre = vector.create(10, -24, -50), radius = 30 },
    { centre = vector.create(-20, -10, 45), radius = 14 },
    { centre = vector.create(60, -28, -30), radius = 34 },
}
for _, hill in hills do
    terrain:FillBall(hill.centre, hill.radius, Grass)
end
-- A steep bank, where the grass gives out.
terrain:FillBlock(vector.create(-70, 6, 40), vector.create(20, 12, 40), Grass)
-- An arch: the hollow under it cannot see the sky, so nothing grows there.
terrain:FillBlock(vector.create(0, 6, 70), vector.create(24, 8, 10), Grass)
terrain:FillBlock(vector.create(0, 2, 70), vector.create(12, 4, 12), 0)

-- **Where things grow**: gentle ground, of the grass material.
local meadow = Instance.new("FoliageLayer")
meadow.Name = "Meadow"
meadow.Density = 6
meadow.SlopeMax = 32
meadow.Clumping = 0.3
meadow.MinSpacing = 0.15
meadow.DrawDistance = 90
meadow.FadeDistance = 25
meadow.Seed = 7
meadow:SetMaterials({ { Material = Grass, Density = 1 } })
meadow.Parent = terrain

-- **What grows**: grass, and a flower in every dozen.
local grass = Instance.new("FoliageMesh")
grass.Name = "Grass"
grass.Mesh = GrassContent
grass.Weight = 12
grass.ScaleMin = 0.7
grass.ScaleMax = 1.3
grass.AlignToNormal = 0.4
grass.WindResponse = 1
grass.Stiffness = 0.8
grass.Parent = meadow

local flower = Instance.new("FoliageMesh")
flower.Name = "Flower"
flower.Mesh = FlowerContent
flower.Weight = 1
flower.ScaleMin = 0.9
flower.ScaleMax = 1.2
flower.Stiffness = 1.5
flower.Parent = meadow

-- A breeze from the west, gusting.
Workspace.GlobalWind = vector.create(6, 0, 2)
Workspace.WindGusts = 0.6
Workspace.WindTurbulence = 0.3

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.Parent = Workspace
Workspace.CurrentCamera = camera

-- Low over the grass, round the meadow, looking a little ahead.
RunService.Heartbeat:Connect(function()
    local angle = RunService.SimTime / LapSeconds * math.pi * 2
    local x = math.cos(angle) * 40
    local z = math.sin(angle) * 40
    local ground = terrain:HeightAt(x, z) or 0
    local ahead = angle + 0.35
    local tx = math.cos(ahead) * 40
    local tz = math.sin(ahead) * 40
    local groundAhead = terrain:HeightAt(tx, tz) or 0
    camera.CFrame = CFrame.lookAt(vector.create(x, ground + 1.8, z), vector.create(tx, groundAhead + 0.8, tz))
end)

print("meadow: grown by rules, drawn by a GPU cull")
