--!strict

-- The field: ground its code writes and paints, as a game's menu does, and
-- a few hundred parts with names and attributes of their own.
local terrain = Instance.new("Terrain")
terrain.VoxelSize = 1
terrain.MinHeight = -8
terrain.MaxHeight = 32
terrain.Parent = workspace

local Size = 384
local columns = Size + 1
local heights = table.create(columns * columns, 0)
for row = 0, Size do
    for column = 0, Size do
        heights[row * columns + column + 1] = 2 + math.sin(column * 0.15) * 2 + math.cos(row * 0.11) * 2
    end
end
terrain:WriteHeights(vector.create(-Size / 2, 0, -Size / 2), columns, heights, 1)
for at = 0, 40 do
    terrain:PaintBall(vector.create(at * 2 - 40, 3, 0), 3, 2)
end

local folder = Instance.new("Folder")
folder.Name = "Pieces"
folder.Parent = workspace
for at = 1, 400 do
    local piece = Instance.new("Part")
    piece.Name = `Piece{at}`
    piece.Anchored = true
    piece.Size = vector.create(0.5, 0.5, 0.5)
    piece.Position = vector.create(at % 20, 6, at // 20)
    piece:SetAttribute("Kind", `kind-{at % 7}`)
    piece.Parent = folder
end

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 20, 40), vector.create(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
