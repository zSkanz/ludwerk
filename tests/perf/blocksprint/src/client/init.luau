--!strict

-- The block sprint: the scene `docs/perf-baselines.md` prices a block world
-- being built around a player against (the voxel-world ledger's B0).
--
-- Asked by the owner on his phone, of a block game made with the engine: "a lot
-- of lag while the game is building the chunks... once it has built them it is
-- smooth, perfect". So this is that, with nothing else in it:
--
--   * **A world from a formula**, so two runs build the same blocks: hills of
--     stone with grass on top, sand where they meet the sea, and a sea of
--     fluid sources up to its level.
--   * **A viewer that runs in a straight line** at sprint speed, for as long as
--     the run lasts, so the world is never finished being built.
--   * **One column of sixteen by sixteen blocks built every four ticks**, the
--     nearest missing one first, and one let go behind for each -- the pace a
--     game's own generator keeps so that a tick stays a tick.
--
-- What is measured is the FRAME while that happens. Run by hand rather than by
-- CTest, for the reason `tests/perf/README.md` gives:
--
--   engine-host tests/perf/blocksprint --headless --frames=2400 --exit --frame-stats
--
-- and read "over 33 ms", the 95th percentile, and the simulation and
-- CPU-drawing parts apart.

local RunService = game:GetService("RunService")
local VoxelService = game:GetService("VoxelService")
local Workspace = game:GetService("Workspace")

local Column = 16
local SeaLevel = 22
-- How many columns round the viewer are kept, as a radius in columns.
local Radius = 7
-- A run, in blocks a second.
local Speed = 7
local TicksPerColumn = 4

local Stone = VoxelService:RegisterBlock("Stone", Color3.new(0.5, 0.5, 0.53))
local Grass = VoxelService:RegisterBlock("Grass", Color3.new(0.3, 0.62, 0.24))
local Sand = VoxelService:RegisterBlock("Sand", Color3.new(0.84, 0.78, 0.55))
local Water = VoxelService:RegisterBlock("Water", Color3.new(0.12, 0.32, 0.62))
VoxelService:SetBlockOpacity(Water, Enum.BlockOpacity.Translucent, 0.4)
VoxelService:SetBlockFluid(Water, 4)

--- The ground's height at a column of blocks: long hills, and shorter ones on
--- them, crossing the sea's level so there is shore.
local function heightAt(x: number, z: number): number
    return math.floor(22 + 9 * math.sin(x * 0.045) * math.cos(z * 0.037) + 3 * math.sin(x * 0.19 + z * 0.13))
end

local Built: { [string]: boolean } = {}

local function keyOf(cx: number, cz: number): string
    return `{cx},{cz}`
end

local function build(cx: number, cz: number)
    for x = cx * Column, cx * Column + Column - 1 do
        for z = cz * Column, cz * Column + Column - 1 do
            local height = heightAt(x, z)
            VoxelService:FillBlocks(vector.create(x, 0, z), vector.create(x, height - 1, z), Stone)
            VoxelService:SetBlock(vector.create(x, height, z), if height <= SeaLevel + 1 then Sand else Grass)
            if height < SeaLevel then
                VoxelService:FillBlocks(vector.create(x, height + 1, z), vector.create(x, SeaLevel, z), Water)
            end
        end
    end
    Built[keyOf(cx, cz)] = true
end

local function letGo(cx: number, cz: number)
    VoxelService:FillBlocks(
        vector.create(cx * Column, 0, cz * Column),
        vector.create(cx * Column + Column - 1, 48, cz * Column + Column - 1),
        0
    )
    Built[keyOf(cx, cz)] = nil
end

local Camera = Instance.new("Camera")
Camera.FieldOfView = 75
Camera.FarPlane = 400
Camera.Parent = Workspace
Workspace.CurrentCamera = Camera

local Travelled = 0
local Tick = 0

RunService.Heartbeat:Connect(function(dt: number)
    Tick += 1
    Travelled += Speed * dt
    local eye = vector.create(8, 46, Travelled)
    Camera.CFrame = CFrame.lookAt(eye, eye + vector.create(0, -0.45, 1))

    if Tick % TicksPerColumn ~= 0 then
        return
    end
    local hereX = math.floor(eye.x / Column)
    local hereZ = math.floor(eye.z / Column)

    -- The nearest column in range that is not there yet.
    local best: { number }? = nil
    local bestDistance = math.huge
    for cx = hereX - Radius, hereX + Radius do
        for cz = hereZ - Radius, hereZ + Radius do
            local distance = (cx - hereX) ^ 2 + (cz - hereZ) ^ 2
            if distance <= Radius * Radius and distance < bestDistance and not Built[keyOf(cx, cz)] then
                best = { cx, cz }
                bestDistance = distance
            end
        end
    end
    if best then
        build(best[1], best[2])
    end

    -- And one behind, past the range with a column to spare.
    for key in Built do
        local parts = string.split(key, ",")
        local cx, cz = tonumber(parts[1]) :: number, tonumber(parts[2]) :: number
        if (cx - hereX) ^ 2 + (cz - hereZ) ^ 2 > (Radius + 1) ^ 2 then
            letGo(cx, cz)
            break
        end
    end
end)
