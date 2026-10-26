--!strict

-- **What a wall hides is not drawn** (ADR 0262): a few streets of houses, a
-- crowd of small things behind them, and a camera at street level turning
-- fast at a crossing. Drawn once with the occlusion culler and once without,
-- every frame must be the same picture: a thing hidden wrongly, or one that
-- appears a frame late as the camera turns past a corner, is a difference.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 14
Lighting.GlobalShadows = true

local ground = Instance.new("Part")
ground.Name = "Ground"
ground.Anchored = true
ground.Size = vector.create(240, 1, 240)
ground.CFrame = CFrame.new(0, -0.5, 0)
ground:SetMaterialParameter("Color", Color3.fromRGB(120, 120, 110))
ground.Parent = workspace

-- Houses in blocks of four, with streets ten metres wide between the
-- blocks: what a camera in a street sees past is the next house.
local HouseSize = vector.create(10, 8, 10)
local Spacing = 26
local houses = Instance.new("Folder")
houses.Name = "Houses"
houses.Parent = workspace
for row = -3, 3 do
    for column = -3, 3 do
        local blockX = column * Spacing
        local blockZ = row * Spacing
        for corner = 0, 3 do
            local house = Instance.new("Part")
            house.Anchored = true
            house.Size = HouseSize
            house.CFrame = CFrame.new(
                blockX + (corner % 2 == 0 and -5.5 or 5.5),
                HouseSize.y * 0.5,
                blockZ + (corner < 2 and -5.5 or 5.5)
            )
            house:SetMaterialParameter("Color", Color3.fromRGB(180 + corner * 15, 150, 130))
            house.Parent = houses
        end
    end
end

-- A crowd of small things in the yards behind the houses, and some in the
-- streets: most hidden from a street, some in plain sight.
local things = Instance.new("Folder")
things.Name = "Things"
things.Parent = workspace
for index = 0, 399 do
    local thing = Instance.new("Part")
    thing.Shape = index % 2 == 0 and Enum.PartShape.Ball or Enum.PartShape.Cylinder
    thing.Anchored = true
    thing.Size = vector.create(1.2, 1.6, 1.2)
    local x = ((index * 37) % 160) - 80
    local z = ((index * 59) % 160) - 80
    thing.CFrame = CFrame.new(x + 0.3, 0.8, z + 0.7)
    thing:SetMaterialParameter("Color", Color3.fromHSV((index % 12) / 12, 0.7, 0.9))
    thing.Parent = things
end

-- At a crossing, turning: a full turn every two seconds, so a corner sweeps
-- past in a few frames.
local camera = Instance.new("Camera")
camera.FieldOfView = 70
camera.NearPlane = 0.1
camera.FarPlane = 400
camera.Parent = workspace
workspace.CurrentCamera = camera

local frame = 0
RunService.PreRender:Connect(function()
    frame += 1
    local angle = frame * (math.pi * 2 / 120)
    local eye = vector.create(Spacing * 0.5, 1.7, Spacing * 0.5)
    camera.CFrame = CFrame.lookAt(eye, eye + vector.create(math.cos(angle), -0.02, math.sin(angle)))
end)
