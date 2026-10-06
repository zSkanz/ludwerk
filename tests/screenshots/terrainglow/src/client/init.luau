--!strict

-- **A terrain layer whose material gives off light glows** (ADR 0113,
-- amended). Flat ground, rock to the left of the picture and lava to the
-- right, and nothing lighting either: no sun, no sky, no ambient. The rock is
-- black. The lava is its material's `Emissive` -- which the ground did not
-- read, so a volcano's seams were as dark as the stone beside them.
--
-- `run_terrain_glow.cmake` photographs it with the full ground and with the
-- fast one (`--terrain-surface=fast`), which is a shader of its own.

local Lighting = game:GetService("Lighting")

local terrain = Instance.new("Terrain")
terrain.Name = "Ground"
terrain.MinHeight = -64
terrain.MaxHeight = 64
terrain:SetLayers({ "asset://materials/rock.material.json", "asset://materials/lava.material.json" })
terrain.Parent = workspace
-- Each half filled with its own layer: a fill does not repaint what is full.
terrain:FillBlock(vector.create(-24, -2, 0), vector.create(48, 4, 96), 1)
terrain:FillBlock(vector.create(24, -2, 0), vector.create(48, 4, 96), 2)

local camera = Instance.new("Camera")
camera.FieldOfView = 55
camera.CFrame = CFrame.lookAt(vector.create(0, 30, 0.01), vector.create(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 12
Lighting.Brightness = 0
Lighting.Ambient = Color3.new(0, 0, 0)
Lighting.OutdoorAmbient = Color3.new(0, 0, 0)
Lighting.EnvironmentDiffuseScale = 0
Lighting.EnvironmentSpecularScale = 0
Lighting.AutoExposure = false
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
