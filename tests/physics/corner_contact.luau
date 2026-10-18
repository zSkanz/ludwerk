--!strict
-- A ball rolled at a small crate's corner stops against the crate as it is
-- drawn: its deepest overlap with the drawn box, measured every tick, is
-- under two millimetres. A box collides with rounded edges, and rounded as
-- much as Jolt's default the ball sank two centimetres into the corner.
local RunService = game:GetService("RunService")

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(20, 1, 20)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = workspace

local HalfCrate = 0.075
local crate = Instance.new("Part")
crate.Anchored = true
crate.Size = vector.create(HalfCrate * 2, HalfCrate * 2, HalfCrate * 2)
crate.Position = vector.create(0, HalfCrate, 0)
crate.Parent = workspace

local Radius = 0.06
local ball = Instance.new("Part")
ball.Shape = Enum.PartShape.Ball
ball.Size = vector.create(Radius * 2, Radius * 2, Radius * 2)
ball.Position = vector.create(0.4, Radius, 0.4)
ball.Parent = workspace

-- How far the ball's surface is outside the drawn crate: negative inside.
local function clearance(): number
    local centre = ball.Position - crate.Position
    local outside = vector.create(
        math.max(math.abs(centre.x) - HalfCrate, 0),
        math.max(math.abs(centre.y) - HalfCrate, 0),
        math.max(math.abs(centre.z) - HalfCrate, 0)
    )
    local distance = vector.magnitude(outside)
    if distance == 0 then
        local x = HalfCrate - math.abs(centre.x)
        local y = HalfCrate - math.abs(centre.y)
        local z = HalfCrate - math.abs(centre.z)
        distance = -math.min(x, y, z)
    end
    return distance - Radius
end

-- **And how far things at rest sink into what they rest on**: a ball and a
-- loose crate on the floor, whose top is at nought -- no more than the five
-- millimetres the solver allows.
local resting = Instance.new("Part")
resting.Shape = Enum.PartShape.Ball
resting.Size = vector.create(Radius * 2, Radius * 2, Radius * 2)
resting.Position = vector.create(-1, 0.5, 0)
resting.Parent = workspace
local loose = Instance.new("Part")
loose.Size = vector.create(HalfCrate * 2, HalfCrate * 2, HalfCrate * 2)
loose.Position = vector.create(1, 0.5, 0)
loose.Parent = workspace

local deepest = math.huge
local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    -- Rolled at the corner once it is in the world, along the diagonal.
    if ticks == 2 then
        ball.LinearVelocity = vector.create(-1.2, 0, -1.2)
    end
    deepest = math.min(deepest, clearance())
    if ticks == 120 then
        local ballSunk = Radius - resting.Position.y
        local crateSunk = HalfCrate - loose.Position.y
        local ok = deepest >= -0.002 and ballSunk <= 0.006 and crateSunk <= 0.006
        local deepestMm = string.format("%.1f", deepest * 1000)
        local ballMm = string.format("%.1f", ballSunk * 1000)
        local crateMm = string.format("%.1f", crateSunk * 1000)
        print(`[corner] deepest={deepestMm} mm ok={ok} resting ball {ballMm} mm, crate {crateMm} mm`)
    end
end)
