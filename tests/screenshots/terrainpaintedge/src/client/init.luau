--!strict

-- **Paint blended until it is the ground, beside paint not yet** (D396): a
-- disc of sand blended over grass at full strength becomes sand ground, with
-- nothing over it; a second disc beside it at nine tenths leaves grass with
-- sand over it. Where the two meet a triangle runs from a corner whose ground
-- is sand to one whose sand is paint -- and the ground under the paint showed
-- through as a green line inside the sand.
--
-- Photographed straight down on that line from 4 m, in the albedo view, the
-- layers' repeat left unbroken so what varies is the paint alone.

local terrain = Instance.new("Terrain")
terrain:SetLayers({
    "asset://materials/grass.material.json",
    "asset://materials/sand.material.json",
})
terrain.MinHeight = -16
terrain.MaxHeight = 16
terrain.Parent = workspace

local Size = 64
terrain:WriteHeights(vector.create(-Size / 2, 0, -Size / 2), Size, table.create(Size * Size, 0), 1)
local blend = Enum.TerrainPaintMode.Blend
terrain:PaintBall(vector.create(0, 0, 0), 6, 2, { Mode = blend, Strength = 1 })
terrain:PaintBall(vector.create(8, 0, 0), 6, 2, { Mode = blend, Strength = 0.9 })

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(6, 4, 0), vector.create(6, 0, 0), vector.create(0, 0, -1))
camera.Parent = workspace
workspace.CurrentCamera = camera
