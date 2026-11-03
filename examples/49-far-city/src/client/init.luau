--!strict
--- A city too far to see all of (ADRs 0261 to 0264): an avenue ten kilometres
--- long, streamed in cells, flown down at street level and then from above.
---
---   * **What a wall hides is not drawn** (ADR 0262). At street level the old
---     town's blocks hide the towers behind them, and a ridge of terrain
---     across the avenue hides what is beyond it. The frame report's
---     "Occlusion culling" line says how many draws a frame were left out;
---     from above, where nothing hides anything, it says the culler rests.
---   * **What the world no longer needs is let go** (ADR 0264). Every cell's
---     towers wear a mesh no other cell wears; the cells behind the camera
---     leave, and their meshes leave the GPU ten seconds later -- the "Let go
---     of" line. And the old town's sixteen facades are kept within a texture
---     budget, set low here: as the town falls behind, the farthest are kept
---     smaller -- the "Textures" line's count.
---   * **A mesh changes its level without being seen to** (ADR 0261). The
---     towers change their levels of detail as the camera passes, fading
---     where the quality allows -- the "Levels of detail" line.
---
--- The world is generated: run `lute examples/49-far-city/tools/generate.luau`
--- once, as the launcher does.
---
---   O      occlusion culling off and on  (`GraphicsService.OcclusionCulling`)
---   T      the texture budget off and on (`GraphicsService.TextureStreamingBudget`)
---   V      the view distance: 0.5, 1, 2  (`GraphicsService.ViewDistance`)
---   Space  pause the flight

local GraphicsService = game:GetService("GraphicsService")
local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local StreamingService = game:GetService("StreamingService")

Lighting.ClockTime = 15
Lighting.FogStart = 1500
Lighting.FogEnd = 6000

-- Low on purpose: sixteen facades are more than this, so the farthest give.
local Budget = 3
GraphicsService.TextureStreamingBudget = Budget

-- --- The old town -------------------------------------------------------------
-- Blocks along the first three kilometres, each wearing a facade, at the
-- road's edge: what a camera in the street sees past is the next block.

local town = Instance.new("Folder")
town.Name = "OldTown"
town.Parent = workspace
local facades: { Material } = {}
for index = 0, 15 do
    table.insert(facades, Material.load(`asset://materials/facade_{index}.material.json`))
end
local block = 0
for x = 20, 3000, 34 do
    for _, side in { -1, 1 } do
        block += 1
        local height = 14 + (block * 37) % 26
        local building = Instance.new("Part")
        building.Name = "Block"
        building.Anchored = true
        building.Size = vector.create(28, height, 22)
        building.CFrame = CFrame.new(x, height / 2, 128 + side * 24)
        building.Material = facades[(block % 16) + 1]
        building.Parent = town
    end
end

-- --- A ridge across the avenue -------------------------------------------------

local terrain = Instance.new("Terrain")
terrain.Name = "Ridge"
terrain.MinHeight = -16
terrain.MaxHeight = 64
terrain:SetLayers({ "engine://terrain/grass", "engine://terrain/rock" })
terrain:FillBlock(vector.create(3600, 8, 128), vector.create(80, 16, 600), 2)
for index = -6, 6 do
    terrain:FillBall(vector.create(3600, 14, 128 + index * 45), 22, 1)
end
terrain.Parent = workspace

-- --- The flight -------------------------------------------------------------------

local camera = Instance.new("Camera")
camera.Name = "Flight"
camera.FieldOfView = 70
camera.FarPlane = 7000
camera.Parent = workspace
workspace.CurrentCamera = camera
StreamingService.LoadRadius = 900
StreamingService.MinRadius = 400
StreamingService:AddFocus(camera)

-- A hundred metres a second, down the street for thirty seconds and then up
-- over the ridge to a hundred and fifty metres. On the simulation's clock, so a
-- headless run flies it too.
local Speed = 100
local flown = 0
local paused = false
RunService.Heartbeat:Connect(function(step: number)
    if not paused then
        flown += step
    end
    local x = 10 + flown * Speed
    local climb = math.clamp((flown - 30) / 12, 0, 1)
    local height = 6 + climb * 144
    local position = vector.create(x, height, 128)
    camera.CFrame = CFrame.lookAt(position, position + vector.create(1, -0.04 - climb * 0.25, 0))
end)

local ViewDistances = { 0.5, 1, 2 }
local view = 2
InputService.InputBegan:Connect(function(input: InputObject, consumed: boolean)
    if consumed then
        return
    end
    if input.KeyCode == Enum.KeyCode.O then
        GraphicsService.OcclusionCulling = not GraphicsService.OcclusionCulling
        print(`[far-city] occlusion culling {if GraphicsService.OcclusionCulling then "on" else "off"}`)
    elseif input.KeyCode == Enum.KeyCode.T then
        local on = GraphicsService.TextureStreamingBudget ~= 0
        GraphicsService.TextureStreamingBudget = if on then 0 else Budget
        print(`[far-city] texture budget {if on then "off" else `{Budget} MiB`}`)
    elseif input.KeyCode == Enum.KeyCode.V then
        view = view % #ViewDistances + 1
        GraphicsService.ViewDistance = ViewDistances[view]
        print(`[far-city] view distance {ViewDistances[view]}`)
    elseif input.KeyCode == Enum.KeyCode.Space then
        paused = not paused
    end
end)

print("[far-city] flying the avenue; the frame report says what was hidden, let go and kept smaller")
