--!strict
--- Water under R10 (ADR 0118, B8's declared verification).
---
--- **What this catches.** Everything that floats is pushed every tick by a sum
--- of waves evaluated at the simulation clock -- sines of positions and time --
--- and dragged towards the water's own motion, 27 points per body, as impulses
--- at points. A last bit of a sine that differs between machines, or a sample
--- order that depends on anything but the world, compounds over a thousand
--- ticks into a different sea state for every crate. So:
---
--- * crates of seeded densities ride an ocean of three waves;
--- * a hull is driven by a push at its stern and a twist about the vertical,
---   the two impulses a game sails with;
--- * and a river runs out into a lake, so the rule for a point two waters
---   cover is exercised too.
---
--- The assertions are `error` calls, and the harness fails a run that logged
--- one -- which is what stops a scenario dying on tick one and having its
--- emptiness recorded as a golden.

local RunService = game:GetService("RunService")

local Rng = Random.new(20260929)

-- --- The sea -------------------------------------------------------------------

local sea = Instance.new("Water")
sea.Name = "Sea"
sea.Shape = Enum.WaterShape.Ocean
sea.Current = vector.create(0.3, 0, -0.1)
sea.Viscosity = 0.6
sea.Parent = workspace

for index, wave in
    {
        { Length = 50, Amplitude = 0.7, Direction = 12, Steepness = 0.3 },
        { Length = 23, Amplitude = 0.35, Direction = -50, Steepness = 0.1 },
        { Length = 11, Amplitude = 0.15, Direction = 100, Steepness = 0 },
    }
do
    local term = Instance.new("WaterWave")
    term.Name = `Wave{index}`
    term.Wavelength = wave.Length
    term.Amplitude = wave.Amplitude
    term.Direction = wave.Direction
    term.Steepness = wave.Steepness
    term.Phase = Rng:NextNumber(0, 6)
    term.Parent = sea
end

-- **Crates in a fixed lattice, of seeded densities**, dropped from above so
-- the first ticks are a splash rather than a rest.
for row = 0, 2 do
    for column = 0, 3 do
        local crate = Instance.new("Part")
        crate.Name = string.format("Crate_%d_%d", row, column)
        crate.Size = vector.create(1.5, 1.2, 1.8)
        crate.Density = Rng:NextNumber(0.25, 0.9)
        crate.CFrame = CFrame.new(-12 + column * 8, 2 + row, -10 + row * 9)
            * CFrame.fromEuler(Rng:NextNumber(-0.5, 0.5), Rng:NextNumber(0, 6), 0, Enum.RotationOrder.YXZ)
        crate.Parent = workspace
    end
end

-- A stone, which sinks, and a buoy that the water is told to leave alone.
local stone = Instance.new("Part")
stone.Name = "Stone"
stone.Size = vector.create(1, 1, 1)
stone.Density = 2.5
stone.Position = vector.create(20, 1, 20)
stone.Parent = workspace

local ignored = Instance.new("Part")
ignored.Name = "NotBuoyant"
ignored.Size = vector.create(1, 1, 1)
ignored.Density = 0.3
ignored.Buoyant = false
ignored.Position = vector.create(24, 1, 20)
ignored.Parent = workspace

-- --- The hull ------------------------------------------------------------------

local hull = Instance.new("Part")
hull.Name = "Hull"
hull.Size = vector.create(2.4, 1.2, 7)
hull.Density = 0.4
hull.Position = vector.create(0, 0.5, 20)
hull.Parent = workspace

local mast = Instance.new("Part")
mast.Name = "Mast"
mast.Size = vector.create(0.2, 4, 0.2)
mast.CanCollide = false
mast.Parent = workspace
local weld = Instance.new("Weld")
weld.Part0 = hull
weld.Part1 = mast
weld.C0 = CFrame.new(0, 2.6, -0.5)
weld.Parent = hull

-- --- A river into a lake, far from the sea's floaters ------------------------

local lake = Instance.new("Water")
lake.Name = "Lake"
lake.Shape = Enum.WaterShape.Box
lake.Position = vector.create(120, 0, 0)
lake.Size = vector.create(30, 6, 30)
lake.SurfaceLevel = 2
lake.Parent = workspace

local river = Instance.new("Water")
river.Name = "River"
river.Shape = Enum.WaterShape.Spline
river.Size = vector.create(6, 3, 0)
river.SurfaceLevel = 2
river.FlowSpeed = 3
river.Parent = workspace
for index, at in { vector.create(80, 0, -20), vector.create(100, 0, -5), vector.create(118, 0, 0) } do
    local point = Instance.new("WaterPoint")
    point.Name = `Point{index}`
    point.Position = at
    point.Parent = river
end

local log = Instance.new("Part")
log.Name = "Log"
log.Size = vector.create(0.6, 0.6, 2.5)
log.Density = 0.5
log.Position = vector.create(82, 2.5, -18)
log.Parent = workspace

-- --- The tick ------------------------------------------------------------------

local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    local frame = hull.CFrame
    -- A push at the stern, low, and a slow turn: `ApplyImpulseAtPosition`
    -- and `ApplyAngularImpulse` into the same trace as the water's own.
    local look = frame.LookVector
    local forward = vector.normalize(vector.create(look.x, 0, look.z))
    local stern = frame:PointToWorldSpace(vector.create(0, -0.3, 3.5))
    hull:ApplyImpulseAtPosition(forward * 0.35, stern)
    if ticks % 240 < 120 then
        hull:ApplyAngularImpulse(vector.create(0, 0.25, 0))
    end

    if ticks == 600 then
        -- Everything that floats is at the surface, give or take its waves;
        -- the stone is on its way down and the ignored one never slowed.
        local surface = sea:GetHeightAt(hull.Position)
        if surface == nil or math.abs(hull.Position.y - surface) > 1.5 then
            error("the hull is not riding the sea")
        end
        if stone.Position.y > -10 then
            error("the stone did not sink")
        end
        if ignored.Position.y > -10 then
            error("a part that is not buoyant was floated")
        end
        if log.Position.x < 92 then
            error("the river did not carry the log")
        end
    end
end)
