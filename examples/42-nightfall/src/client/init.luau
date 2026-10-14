--!strict
--[[
	Nightfall in a courtyard (ADRs 0203, 0204, 0206): the sun goes down over
	twenty seconds, a walker crosses to a doorway, and the camera stays at her
	shoulder -- and none of it is in a room a box could be drawn round.

	What does each job:

	- **The light in the open** is one `IrradianceVolume` that `FollowsCamera`:
	  the part under it gives its size, and it stands round the camera as
	  three levels, each twice the last. The red wall reddens the ground
	  beside it, and goes on doing it as the camera walks.
	- **The tone is the authored one.** `Mode = BounceOnly`: `Lighting.Ambient`
	  lights the place exactly as typed, and the volume adds only what bounces
	  off the wall and off what the torch lights. `Tint` is a look on top.
	- **The light never stands still**, and that is fine: the sun moves every
	  frame and the torch flickers every frame. The volume is `Dynamic`; its
	  levels are lit again one at a time, each by the light there was when its
	  turn began, inside `GraphicsService.GlobalIlluminationBudget`
	  milliseconds a frame. `IsReady` goes true once and stays true.
	- **The sequence** (`content/sequences/nightfall.sequence.json`) keys the
	  hour on `Lighting` itself -- a service is bound like anything else --
	  fades in from black, brings the bars of a wider picture in, shows a
	  caption nobody says, and keeps the camera two metres behind the walker
	  with `relativeTo`, wherever she has got to.
	- **It stops at a hold** eight seconds in, until somebody decides: `Held`
	  says where, `HoldReached` fires, `Release` goes on. The walker goes on
	  breathing meanwhile; the sun does not go on setting.
	- **It has three shots**, marked. `N` skips to the next, leaving the hour
	  where the sequence would have left it.

	Space or A: go on from the hold.   N or X: the next shot.   Enter: again.
]]

local subtitles = require("@engine/subtitles")

local GraphicsService = game:GetService("GraphicsService")
local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 17
Lighting.GeographicLatitude = 30
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
-- The place's own tone, typed by hand: it is kept.
Lighting.Ambient = Color3.fromRGB(34, 38, 52)
Lighting.OutdoorAmbient = Color3.fromRGB(58, 62, 78)

-- A millisecond more than the default a frame: three levels and a torch.
GraphicsService.GlobalIlluminationBudget = 2.5

-- --- The courtyard ---------------------------------------------------------------

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

local Stone = Color3.fromRGB(150, 146, 138)
slab("Ground", vector.create(160, 1, 160), vector.create(0, -0.5, 0), Stone)
-- The wall the last of the sun is on, and a doorway through it.
slab("WallWest", vector.create(14, 7, 1), vector.create(-9, 3.5, -12), Color3.fromRGB(196, 72, 58))
slab("WallEast", vector.create(14, 7, 1), vector.create(9, 3.5, -12), Color3.fromRGB(196, 72, 58))
slab("Lintel", vector.create(4, 2.6, 1), vector.create(0, 5.7, -12), Color3.fromRGB(196, 72, 58))
-- A canopy beside the door: the ground under it is lit by the wall, and by
-- the torch that hangs from it.
slab("Canopy", vector.create(8, 0.3, 5), vector.create(-8, 2.8, -8.5), Color3.fromRGB(92, 78, 64))
for index = 0, 5 do
    slab(
        `Pillar{index}`,
        vector.create(1.2, 5, 1.2),
        vector.create(-18 + index * 7.2, 2.5, 10),
        Color3.fromRGB(176, 170, 158)
    )
end

local torchHolder =
    slab("Torch", vector.create(0.2, 0.4, 0.2), vector.create(-8, 2.3, -8.5), Color3.fromRGB(60, 44, 30))
torchHolder.CanCollide = false
torchHolder.CanQuery = false
torchHolder.CastShadow = false
local torch = Instance.new("PointLight")
torch.Color = Color3.fromRGB(255, 176, 96)
torch.Brightness = 2.4
torch.Range = 14
torch.Parent = torchHolder

-- The walker: a body a camera key can stand behind.
local anna = Instance.new("Part")
anna.Name = "Anna"
anna.Size = vector.create(0.8, 1.8, 0.6)
anna.CFrame = CFrame.new(-2, 0.9, 16)
anna:SetMaterialParameter("Color", Color3.fromRGB(232, 228, 220))
anna.Anchored = true
anna.CanCollide = false
anna.Parent = workspace

-- --- The light in the open ---------------------------------------------------------

local size = Instance.new("Part")
size.Name = "OpenLight"
size.Size = vector.create(40, 16, 40)
size:SetMaterialParameter("Transparency", 1)
size.CanCollide = false
size.CanQuery = false
size.Anchored = true
size.Parent = workspace

local open = Instance.new("IrradianceVolume")
open.FollowsCamera = true
open.Levels = 3
open.Spacing = 2
open.Dynamic = true
open.Mode = Enum.IrradianceMode.BounceOnly
open.Tint = Color3.new(1, 0.94, 0.86)
open.Parent = size

open.Completed:Connect(function()
    print("[nightfall] the open light is worked out")
end)

-- --- What is on the screen ---------------------------------------------------------

local hud = Instance.new("ScreenGui")
hud.Name = "Hud"
hud.ReferenceHeight = 720
hud.Parent = UIService

local hint = Instance.new("TextLabel")
hint.AnchorPoint = Vector2.new(0.5, 1)
hint.Position = UDim2.new(0.5, 0, 1, -110)
hint.Size = UDim2.new(1, -80, 0, 28)
hint.BackgroundTransparency = 1
hint.TextSize = 18
hint.TextColor = Color3.fromRGB(226, 230, 240)
hint.Parent = hud

subtitles.mount()

-- --- The sequence -----------------------------------------------------------------

local nightfall = Instance.new("SequencePlayer")
nightfall.Name = "Nightfall"
nightfall.Sequence = "asset://sequences/nightfall.sequence.json" :: Content
local function bind(name: string, target: Instance)
    local binding = Instance.new("SequenceBinding")
    binding.Name = name
    binding.Target = target
    binding.Parent = nightfall
end
-- A service is bound as anything is: the hour is `Lighting`'s to key.
bind("Lighting", Lighting)
bind("Anna", anna)
nightfall.Parent = workspace

local function say()
    if nightfall.Held ~= "" then
        hint.Text = LocalizationService:Translate("hud.held")
    elseif nightfall.Playing then
        hint.Text = LocalizationService:Translate("hud.playing")
    else
        hint.Text = LocalizationService:Translate("hud.over")
    end
end

nightfall.HoldReached:Connect(function(name: string)
    print(`[nightfall] held at {name}, {nightfall.TimePosition} of {nightfall.Length} seconds`)
    say()
end)
nightfall.MarkerReached:Connect(function(name: string)
    print(`[nightfall] shot {name}`)
    say()
end)
nightfall.Finished:Connect(function(skipped: boolean)
    print(`[nightfall] over, skipped={skipped}, the hour is {Lighting.ClockTime}`)
    say()
end)

-- The walk: hers, not the sequence's -- the camera key is in her frame, so
-- it is behind her shoulder wherever this has put her.
local frame = 0
RunService.PreSimulation:Connect(function(dt: number)
    frame += 1
    -- She waits where the sequence waits.
    if nightfall.Playing and anna.Position.z > -9 then
        anna.CFrame =
            CFrame.lookAt(anna.Position + vector.create(0.2, 0, -1.4) * dt, anna.Position + vector.create(2, 0, -14))
    end
    -- A flame: another brightness every tick.
    torch.Brightness = 2.4 + math.sin(frame / 5) * 0.5 + math.sin(frame / 1.7) * 0.2
end)

local function again()
    Lighting.ClockTime = 17
    anna.CFrame = CFrame.new(-2, 0.9, 16)
    nightfall.TimePosition = 0
    nightfall:Play()
    say()
end

InputService.InputBegan:Connect(function(input: InputObject, processed: boolean)
    if processed then
        return
    end
    if input.KeyCode == Enum.KeyCode.Space or input.KeyCode == Enum.KeyCode.ButtonSouth then
        nightfall:Release()
    elseif input.KeyCode == Enum.KeyCode.N or input.KeyCode == Enum.KeyCode.ButtonWest then
        if not nightfall:SkipToNext() then
            nightfall:Skip()
        end
    elseif input.KeyCode == Enum.KeyCode.Return or input.KeyCode == Enum.KeyCode.ButtonStart then
        again()
    end
    say()
end)

-- The game's own camera, which the sequence takes as a view and gives back.
local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.CFrame = CFrame.lookAt(vector.create(10, 6, 24), vector.create(-2, 2, -6))
camera.Parent = workspace
workspace.CurrentCamera = camera

print(`[nightfall] {#nightfall:GetMarkers()} shots, {nightfall.Length} seconds`)
nightfall:Play()
say()
