--!strict

-- **How much of a texture is loaded** (D609). Three pictures of 256 pixels:
--
--   `wall.png`   a material's, on a part. What `GraphicsService.TextureQuality`
--                is for: at `Low` it is loaded without its two largest levels.
--   `sprite.png` a sprite's. Drawn at its own size: whole at every quality.
--   `both.png`   a material's AND a sprite's. The sprite's word wins, whoever
--                asked first: whole.
--
-- The driver runs this at `High` and at `Low` and reads the frame report: at
-- `Low` one texture is on the card smaller than its file, and at `High` none.

local function material(name: string): Material
    return Material.load(`asset://materials/{name}.material.json` :: any)
end

local function wall(x: number, wearing: Material)
    local part = Instance.new("Part")
    part.Anchored = true
    part.Size = vector.create(3, 3, 0.2)
    part.CFrame = CFrame.new(x, 1.5, 0)
    part.Material = wearing
    part.Parent = workspace
end

local function sprite(x: number, picture: string)
    local made = Instance.new("Part2D")
    made.Position = Vector2.new(x, 5)
    made.Size = Vector2.new(2, 2)
    made.Image = `asset://textures/{picture}.png` :: Content
    made.Anchored = true
    made.CanCollide = false
    made.Parent = workspace
end

wall(-2, material("wall"))
wall(2, material("both"))
sprite(-2, "sprite")
sprite(2, "both")

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 3, 12), vector.create(0, 3, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
