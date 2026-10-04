--!strict
--[[
	Particles simulated on the GPU, and what they meet (ADR 0160).

	Three emitters over flat terrain, each raining bright sparks straight down
	that stick where they land, seen from above at a slant:

	  left    Collision Terrain    -- they land on the ground's height map.
	  middle  Collision SceneDepth -- they land on the picture's depth.
	  right   Collision None       -- they fall through and are never seen again.

	`run_particles_gpu_gate.cmake` asks that the ground under the first two is
	brighter than the ground under the third: sparks lying on it.

	`tests/screenshots/particlesgpumany` is the same ground under one emitter of
	a hundred thousand sparks, which the driver times.
]]

local Lighting = game:GetService("Lighting")

Lighting.ClockTime = 12

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(0, 22, 26), vector.create(0, 0, 0))

local ground = Instance.new("Terrain")
ground.Parent = workspace
local columns = 96
local heights = table.create(columns * columns, 0)
ground:WriteHeights(vector.create(-48, 0, -48), columns, heights)

local function sparks(x: number, collision: EnumParticleCollision, rate: number): ParticleEmitter
    -- Born at a point in the air: an attachment of a part buried out of sight,
    -- turned so its up is down.
    local buried = Instance.new("Part")
    buried.Size = vector.create(1, 1, 1)
    buried.CFrame = CFrame.new(x, -40, 0)
    buried.Anchored = true
    buried.CanCollide = false
    buried.Parent = workspace
    local anchor = Instance.new("Attachment")
    anchor.CFrame = CFrame.new(0, 52, 0) * CFrame.fromEuler(math.pi, 0, 0)
    anchor.Parent = buried
    local emitter = Instance.new("ParticleEmitter")
    emitter.Simulation = Enum.ParticleSimulation.Gpu
    emitter.Rate = rate
    emitter.Lifetime = 5
    emitter.Speed = 6
    emitter.SpreadAngle = 12
    emitter.Acceleration = vector.create(0, -9.81, 0)
    emitter.Size = 0.35
    emitter.SizeEnd = 0.35
    emitter.Transparency = 0
    emitter.TransparencyEnd = 0
    emitter.Color = Color3.fromRGB(255, 220, 120)
    emitter.ColorEnd = Color3.fromRGB(255, 220, 120)
    emitter.LightEmission = 1
    emitter.Brightness = 2
    emitter.Shape = Enum.ParticleShape.Disc
    emitter.Collision = collision
    emitter.CollisionResponse = Enum.ParticleCollisionResponse.Stick
    emitter.Parent = anchor
    return emitter
end

sparks(-10, Enum.ParticleCollision.Terrain, 400)
sparks(0, Enum.ParticleCollision.SceneDepth, 400)
sparks(10, Enum.ParticleCollision.None, 400)
