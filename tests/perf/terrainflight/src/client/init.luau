--!strict

-- **The terrain flight** (terrain audit T0): the terrain gallery's shapes,
-- flown over from 1200 m down to 60 m and back once every 1500 frames -- the
-- flight the audit measured the owner's place with, where every level of
-- detail is rebuilt on the way. Measured by hand (`tests/perf/README.md`);
-- the numbers are in `docs/perf-baselines.md`.
--
-- The shapes are the gallery's (`tests/screenshots/terraingallery`), copied:
-- a project cannot require another's script.

local RunService = game:GetService("RunService")

-- --- The shapes -----------------------------------------------------------------

local Grass = 1
local Sand = 2
local Rock = 3

local terrain = Instance.new("Terrain")
terrain.Name = "Gallery"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({
    "engine://terrain/grass",
    "engine://terrain/sand",
    "engine://terrain/rock",
})
terrain.Parent = workspace

-- Two slabs side by side, floating: ground that does not reach the floor, the
-- owner's own kind. A 4 m one to the west, a 2 m one to the east.
terrain:FillBlock(vector.create(-48, -2, 0), vector.create(96, 4, 160), Grass)
terrain:FillBlock(vector.create(48, -1, 0), vector.create(96, 2, 160), Grass)

-- Balls of radius 2, 4 and 8 on the 4 m slab, sunk a little way in.
terrain:FillBall(vector.create(-84, 1.2, -48), 2, Grass)
terrain:FillBall(vector.create(-70, 2.4, -48), 4, Grass)
terrain:FillBall(vector.create(-48, 4.8, -48), 8, Grass)
-- Two balls merged into one shape.
terrain:FillBall(vector.create(-26, 3, -48), 4, Grass)
terrain:FillBall(vector.create(-20, 3, -48), 4, Grass)
-- One floating clear of the ground.
terrain:FillBall(vector.create(-8, 12, -48), 4, Grass)

-- An overhang: a block, with a ball taken out of the foot of its east face.
terrain:FillBlock(vector.create(-72, 6, 24), vector.create(16, 12, 16), Rock)
terrain:FillBall(vector.create(-64, 5, 24), 4, 0)
-- A cave: a mound with a tunnel through it, east to west.
terrain:FillBlock(vector.create(-32, 6, 24), vector.create(24, 12, 20), Rock)
for x = -46, -18, 2 do
    terrain:FillBall(vector.create(x, 4, 24), 3, 0)
end

-- A 45-degree slope on the 2 m slab, rising to the north: each metre across
-- one metre higher, as columns.
local columns = 24
local heights = {}
for z = 0, 23 do
    for x = 0, columns - 1 do
        heights[z * columns + x + 1] = z + 1
    end
end
terrain:WriteHeights(vector.create(20, 0, 20), columns, heights, Grass)

-- A painted area of two materials.
terrain:PaintBall(vector.create(40, 0, -40), 8, Sand)
terrain:PaintBall(vector.create(52, 0, -40), 6, Rock)

-- **Ground with no material**: a second terrain with no layers, behind the
-- first as the cameras look -- plain matte grey (ADR 0113).
local plain = Instance.new("Terrain")
plain.Name = "Plain"
plain.MinHeight = -64
plain.MaxHeight = 64
plain.Parent = workspace
plain:FillBlock(vector.create(0, -2, -144), vector.create(64, 4, 48), 1)
plain:FillBall(vector.create(0, 2, -144), 6, 1)

-- --- The flight ------------------------------------------------------------------

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.NearPlane = 0.1
camera.FarPlane = 5000
camera.Parent = workspace
workspace.CurrentCamera = camera

local Center = vector.create(0, 0, -20)
local Period = 1500

local function place()
    local frame = RunService.SimTime * 60
    local t = (frame % Period) / Period
    local down = 1 - math.abs(2 * t - 1)
    local height = 1200 - (1200 - 60) * down
    local eye = Center + vector.create(height * 0.9, height, height * 1.2)
    camera.CFrame = CFrame.lookAt(eye, Center)
end

place()
RunService.Heartbeat:Connect(place)
