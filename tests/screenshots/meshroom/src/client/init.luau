--!strict
--- A room that is ONE mesh (ADR 0203): its walls, its ceiling, its tiled
--- floor, a window and a door, in a single file -- the annex of
--- `examples/39-interior`, as a building from a modelling program is. To the
--- physics world it is one hull with every opening shut. The light an
--- `IrradianceVolume` works out is cast against its triangles instead, so the
--- sun and the sky come in through the openings it has.
---
--- The camera is inside. A dim panel on the east wall is the measure: the
--- wall beside it must be BRIGHTER than it, which it is not when the mesh is
--- asked by its hull -- every point of the volume is then inside a solid, and
--- the room is black. See `run_mesh_room_gate.cmake`.

local Lighting = game:GetService("Lighting")
Lighting.ClockTime = 13
Lighting.GeographicLatitude = 42
-- A fixed exposure three stops up: the gate holds the exposure still, and an
-- interior is dim by daylight's measure.
Lighting.ExposureCompensation = 3

local ground = Instance.new("Part")
ground.Name = "Ground"
ground.Size = vector.create(40, 0.5, 40)
ground.Position = vector.create(0, -0.5, 0)
ground:SetMaterialParameter("Color", Color3.fromRGB(120, 124, 110))
ground.Anchored = true
ground.Parent = workspace

local annex = Instance.new("MeshPart")
annex.Name = "Annex"
annex.MeshContent = "asset://models/annex.gltf" :: Content
annex.Anchored = true
annex.Parent = workspace

local box = Instance.new("Part")
box.Name = "AnnexRoom"
box.Size = vector.create(6, 3, 6)
box.Position = vector.create(0, 1.5, 0)
box:SetMaterialParameter("Transparency", 1)
box.CanCollide = false
box.CanQuery = false
box.Anchored = true
box.Parent = workspace
local volume = Instance.new("IrradianceVolume")
volume.Spacing = 1
volume.Bounces = 2
volume.Parent = box

-- The measure: a panel that glows a little, flat on the east wall.
local dim = Instance.new("Part")
dim.Name = "Dim"
dim.Size = vector.create(0.05, 0.6, 0.6)
dim.Position = vector.create(2.97, 1.6, -1.0)
dim:SetMaterialParameter("Color", Color3.new(0, 0, 0))
dim:SetMaterialParameter("Emissive", Color3.new(0.008, 0.008, 0.008))
dim.Anchored = true
dim.CanCollide = false
dim.CanQuery = false
dim.Parent = workspace

local camera = Instance.new("Camera")
camera.Name = "InTheAnnex"
camera.FieldOfView = 80
camera.Parent = workspace
workspace.CurrentCamera = camera
-- From the north-west corner, looking at the east wall.
camera.CFrame = CFrame.lookAt(vector.create(-2.2, 1.6, -2.2), vector.create(3, 1.5, 0.2))
