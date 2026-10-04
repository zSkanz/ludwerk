--!strict
--[[
	A hundred thousand sparks alive at once, simulated on the GPU (ADR 0160):
	twenty thousand a second living five, bouncing on the terrain and on what
	is seen. `run_particles_gpu_gate.cmake` times the frame this makes, in a
	window with the display's sync off, and holds it to sixty frames a second.
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
ground:WriteHeights(vector.create(-48, 0, -48), columns, table.create(columns * columns, 0))

-- Born at a point in the air: an attachment of a part buried out of sight.
local buried = Instance.new("Part")
buried.Size = vector.create(1, 1, 1)
buried.CFrame = CFrame.new(0, -40, 0)
buried.Anchored = true
buried.CanCollide = false
buried.Parent = workspace
local anchor = Instance.new("Attachment")
anchor.CFrame = CFrame.new(0, 48, 0)
anchor.Parent = buried

local emitter = Instance.new("ParticleEmitter")
emitter.Simulation = Enum.ParticleSimulation.Gpu
emitter.Rate = 20000
emitter.Lifetime = 5
emitter.Speed = 12
emitter.SpreadAngle = 55
emitter.Acceleration = vector.create(0, -9.81, 0)
emitter.Size = 0.1
emitter.SizeEnd = 0.1
emitter.Transparency = 0
emitter.TransparencyEnd = 1
emitter.Color = Color3.fromRGB(255, 220, 120)
emitter.ColorEnd = Color3.fromRGB(255, 90, 30)
emitter.LightEmission = 1
emitter.Brightness = 2
emitter.Shape = Enum.ParticleShape.Disc
emitter.Collision = Enum.ParticleCollision.Both
emitter.CollisionResponse = Enum.ParticleCollisionResponse.Bounce
emitter.Bounce = 0.45
emitter.Friction = 0.3
emitter.Parent = anchor
