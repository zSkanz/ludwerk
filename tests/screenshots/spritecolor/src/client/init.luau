--!strict

-- Are a sprite's colours on the screen the colours it was given? (ADR 0153)
--
-- An orthographic camera twenty metres wide and ten tall over squares of two
-- metres, each a claim `run_sprite_color_gate.cmake` probes at its middle:
--
--   top row, left to right, `ExactColor` as it defaults -- an orange, white,
--   a mid grey, a cyan, a near black, and a picture of one flat green: each
--   reads back as its own bytes, to within one step.
--
--   bottom row -- the same orange with `ExactColor` off, which the scene's
--   exposure and tone curve move like any lit thing; a white square under a
--   half-transparent black one, both exact, which is the mix a painter would
--   expect of two colours for a screen; and a tilemap's tile, exact as well.
--
-- `Lighting` is untouched: the claim is about the defaults. Anchored
-- throughout, so nothing moves between runs.

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.new(0, 0, 30)

local function square(x: number, y: number, color: Color3): Part2D
    local created = Instance.new("Part2D")
    created.Position = Vector2.new(x, y)
    created.Size = Vector2.new(2, 2)
    created.Color = color
    created.Anchored = true
    created.CanCollide = false
    created.Parent = workspace
    return created
end

square(-8, 2, Color3.fromRGB(200, 80, 40))
square(-5, 2, Color3.fromRGB(255, 255, 255))
square(-2, 2, Color3.fromRGB(128, 128, 128))
square(1, 2, Color3.fromRGB(10, 200, 250))
square(4, 2, Color3.fromRGB(6, 4, 2))
local picture = square(7, 2, Color3.fromRGB(255, 255, 255))
picture.Image = "asset://sprites/flat.png" :: Content
picture.Filter = Enum.TextureFilter.Nearest

local lit = square(-8, -2, Color3.fromRGB(200, 80, 40))
lit.ExactColor = false

square(-4, -2, Color3.fromRGB(255, 255, 255))
local veil = square(-4, -2, Color3.fromRGB(0, 0, 0))
veil.Transparency = 0.5
veil.ZIndex = 1

local tiles = Instance.new("Tilemap2D")
tiles.Tileset = "asset://sprites/flat.png" :: Content
tiles.TileSize = Vector2.new(8, 8)
tiles.CellSize = 2
tiles.Collides = false
tiles:SetCell(1, -2, 1)
tiles.Parent = workspace
