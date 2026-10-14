--!strict
--- The thin wall (ADR 0203): a room open to the sky and a room shut on every
--- side, with ten centimetres of wall between them and no door. Each has an
--- `IrradianceVolume` of its own, the two boxes meeting inside the wall.
---
--- The camera is in the shut room. All the light there is in it comes off one
--- bright panel on its north wall; a second, dim panel beside it is the
--- measure: the wall it shares with the bright room must stay darker than
--- that, which it does not if a single point of the bright room's light
--- comes through. See `run_thin_wall_gate.cmake`.

local Lighting = game:GetService("Lighting")
Lighting.ClockTime = 12
Lighting.GeographicLatitude = 30

local Plaster = Color3.fromRGB(204, 204, 204)
local function slab(name: string, size: vector, position: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Anchored = true
    part.Parent = workspace
    return part
end

-- Twelve metres by six, three high; the thin wall across the middle.
slab("Floor", vector.create(12.6, 0.5, 6.6), vector.create(0, -0.25, 0), Plaster)
slab("North", vector.create(12.6, 3, 0.3), vector.create(0, 1.5, -3.15), Plaster)
slab("South", vector.create(12.6, 3, 0.3), vector.create(0, 1.5, 3.15), Plaster)
slab("West", vector.create(0.3, 3, 6), vector.create(-6.15, 1.5, 0), Plaster)
slab("East", vector.create(0.3, 3, 6), vector.create(6.15, 1.5, 0), Plaster)
-- **The thin wall has no body at all**: nothing collides with it, touches it
-- or finds it with a ray, so the physics world does not hold it. It is drawn
-- and it does not move, and that is what blocks light: the volumes ask it by
-- its own shape.
local thin = slab("Thin", vector.create(0.1, 3, 6), vector.create(0, 1.5, 0), Plaster)
thin.CanCollide = false
thin.CanQuery = false
thin.CanTouch = false
-- A roof over the east room only: the west one is open to the sun.
slab("Roof", vector.create(6.35, 0.3, 6.6), vector.create(3.125, 3.15, 0), Plaster)

local function room(name: string, centreX: number)
    local box = slab(name, vector.create(6, 3, 6), vector.create(centreX, 1.5, 0), Plaster)
    box:SetMaterialParameter("Transparency", 1)
    box.CanCollide = false
    box.CanQuery = false
    local volume = Instance.new("IrradianceVolume")
    volume.Spacing = 1
    volume.Bounces = 2
    volume.Parent = box
end
room("Open", -3)
room("Shut", 3)

-- The shut room's only light, and the measure beside it.
local bright = slab("Bright", vector.create(1, 1, 0.1), vector.create(4.6, 1.2, -2.95), Color3.new(0, 0, 0))
bright:SetMaterialParameter("Emissive", Color3.new(2, 2, 2))
local dim = slab("Dim", vector.create(0.6, 0.6, 0.1), vector.create(2.0, 1.2, -2.95), Color3.new(0, 0, 0))
dim:SetMaterialParameter("Emissive", Color3.new(0.08, 0.08, 0.08))

local camera = Instance.new("Camera")
camera.Name = "InTheShutRoom"
camera.FieldOfView = 80
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(5.6, 1.6, 2.6), vector.create(0.2, 1.0, -1.6))
