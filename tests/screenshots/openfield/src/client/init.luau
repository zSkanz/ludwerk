--!strict
--- Light that bounces in the open (ADR 0206): no room to draw a box round,
--- so the volume is carried with the camera -- `FollowsCamera`, three levels,
--- each twice the last.
---
--- Two places three hundred and twenty metres apart, each the same: a red wall the sun
--- is on, and beside it a canopy with the ground under it in shadow. The sky
--- and the flat ambient are put out, so that ground is lit by one thing only:
--- what the wall gives back. With no volume it is black.
---
--- The scene data chooses the picture (`run_open_field_gate.cmake`):
---
---   {}                      the first place, no volume: black under the canopy;
---   {"follow": true}        the first place, lit by the wall;
---   {"follow": true, "site": 2}
---                           the second place, three hundred and twenty metres on: lit
---                           the same, by the same volume, which is there
---                           because the camera is;
---   {"follow": true, "walk": true}
---                           the camera goes from sixty metres short of the
---                           first place to it, a step a frame: the levels
---                           are carried after it and the place is lit when
---                           it arrives;
---   {"tiles": true}         no camera-following volume: one box volume over
---                           the first place too large for one volume at its
---                           spacing, worked out as tiles.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

type Asked = { follow: boolean?, site: number?, walk: boolean?, tiles: boolean? }
local asked = (SceneService:GetLoadData() or {}) :: Asked

Lighting.ClockTime = 12
Lighting.GeographicLatitude = 20
Lighting.EnvironmentDiffuseScale = 0
Lighting.EnvironmentSpecularScale = 0
Lighting.Ambient = Color3.new(0, 0, 0)
Lighting.OutdoorAmbient = Color3.new(0, 0, 0)

local function slab(name: string, size: vector, position: vector, color: Color3)
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Anchored = true
    part.Parent = workspace
end

slab("Ground", vector.create(800, 1, 200), vector.create(150, -0.5, 0), Color3.new(0.5, 0.5, 0.5))
-- (Three hundred and twenty: a whole number of every level's steps, so
-- the levels stand on the second place exactly as they stand on the first.)
local Sites = { 0, 320 }
for index, x in Sites do
    -- The wall faces south, where the sun is; the canopy stands south of it,
    -- two and a half metres up, and shades the ground under itself.
    slab(`Wall{index}`, vector.create(12, 8, 1), vector.create(x, 4, -3), Color3.new(0.9, 0.1, 0.1))
    slab(`Canopy{index}`, vector.create(12, 0.3, 5), vector.create(x, 2.5, 3.5), Color3.new(0.5, 0.5, 0.5))
end

if asked.follow or asked.tiles then
    local box = Instance.new("Part")
    box.Name = "Light"
    box:SetMaterialParameter("Transparency", 1)
    box.CanCollide = false
    box.CanQuery = false
    box.Anchored = true
    local volume = Instance.new("IrradianceVolume")
    volume.Mode = Enum.IrradianceMode.BounceOnly
    volume.Bounces = 1
    if asked.tiles then
        -- Seventy-two metres at two between points is thirty-six along a
        -- side, where a volume holds thirty-two: two tiles, meeting at the
        -- middle of the picture.
        box.Size = vector.create(72, 6, 40)
        box.Position = vector.create(0, 3, 0)
        volume.Spacing = 2
    else
        -- Small levels, so the gate is quick on a machine whose rays are
        -- slow: four hundred points each, twenty, forty and eighty metres.
        box.Size = vector.create(20, 8, 20)
        volume.Spacing = 2
        volume.FollowsCamera = true
        volume.Levels = 3
    end
    box.Parent = workspace
    volume.Parent = box
    volume.Completed:Connect(function()
        print("open field: the volume is worked out")
    end)
end

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.Parent = workspace
workspace.CurrentCamera = camera

-- Looking north at the wall from under the south edge of the canopy.
local function stand(x: number)
    camera.CFrame = CFrame.lookAt(vector.create(x, 1.7, 9), vector.create(x, 0.6, 0))
end
local site = Sites[asked.site or 1]
if asked.walk then
    local frame = 0
    local Steps = 240
    stand(site - 60)
    RunService.PreRender:Connect(function()
        frame += 1
        stand(site - 60 * math.max(0, 1 - frame / Steps))
    end)
else
    stand(site)
end
