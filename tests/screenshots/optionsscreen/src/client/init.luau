--!strict
--[[
	The scene for `options_screen_gate` (ADR 0147, section 7): a floor, a row of
	blocks that cast on it and a few that glow, still -- and `@engine/settings`
	open over it, stepped by this script the way a player's presses step it.

	The driver takes a picture at each of these moments:

	- frames 6, 14, 22 and 30: the world alone at `Low`, `Medium`, `High` and
	  `Ultra`, each chosen by a step of the screen's own quality row. No two
	  neighbours may be the same picture, and the first and the last must be
	  the picture of a game started at that level;
	- frame 38: the screen itself, on its first page;
	- frame 46: its second page.

	**The exposure is the world's and fixed** (`Lighting.AutoExposure` off),
	so a picture a few frames after a change is the picture of that level, and
	not of the meter catching up with it. The first version waited two seconds
	at each level for the meter, and on a machine that draws in software it
	ran past its fifteen minutes.

	The colours of the theme are flat and far apart so that a probe can say
	which thing it landed on.
]]

local settings = require("@engine/settings")

local GraphicsService = game:GetService("GraphicsService")
local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 15
Lighting.AutoExposure = false

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Size = Vector3.new(60, 1, 60)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(120, 124, 130))
floor.Anchored = true
floor.Parent = workspace

for index = 1, 7 do
    local block = Instance.new("Part")
    block.Name = `Block{index}`
    -- Thin and tall, so the shadow map's resolution shows in what they cast.
    block.Size = Vector3.new(0.4, 2 + index * 0.6, 0.4)
    block.CFrame = CFrame.new((index - 4) * 2.2, block.Size.y / 2, -2 - index * 1.5)
    block:SetMaterialParameter("Color", Color3.fromRGB(200, 90, 60))
    block.Anchored = true
    block.Parent = workspace
end

-- Three that glow: bloom is on at every level but the lowest.
for index = 1, 3 do
    local lamp = Instance.new("Part")
    lamp.Name = `Lamp{index}`
    lamp.Size = Vector3.new(1, 1, 1)
    lamp.CFrame = CFrame.new((index - 2) * 4, 1.5, 2)
    lamp:SetMaterialParameter("Color", Color3.fromRGB(255, 240, 200))
    lamp:SetMaterialParameter("Emissive", Color3.new(12, 10, 7))
    lamp.Anchored = true
    lamp.Parent = workspace
end

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(Vector3.new(0, 6, 14), Vector3.new(0, 1.5, -4))
camera.Parent = workspace
workspace.CurrentCamera = camera

local screen = settings.open({
    -- One display or three, the picture is the same.
    Hide = { "Monitor" },
    Theme = {
        Backdrop = Color3.fromRGB(10, 10, 10),
        BackdropTransparency = 0,
        Panel = Color3.fromRGB(40, 40, 60),
        Raised = Color3.fromRGB(90, 90, 90),
        Accent = Color3.fromRGB(0, 200, 0),
        Text = Color3.fromRGB(255, 255, 255),
        Dim = Color3.fromRGB(200, 200, 200),
    },
})
-- Out of the way while the world is what is photographed.
screen.Gui.Enabled = false

-- Down to the first stop, the machine's own, and up one: `Low`. With a level
-- pinned by the command line no step moves it, so the walk is counted.
for _ = 1, 4 do
    screen:Step("QualityLevel", -1)
end
screen:Step("QualityLevel", 1)

-- **And the walk says where it is**, two frames before each picture: the
-- driver holds the picture named `ultra` to a line that says `Ultra`. For
-- three weeks it was not: the driver pins one anti-aliasing for every
-- picture, a pinned setting made every level but one read `Custom`, the row
-- steps from `Custom` to the machine's own level, and the walk went `Low`,
-- `High`, `Low`, `High` under four file names.
local tick = 0
RunService.Heartbeat:Connect(function()
    tick += 1
    if tick % 8 == 4 and tick < 32 then
        print(`options-screen: the row is at {GraphicsService.QualityLevel.Name}`)
    end
    if tick == 8 or tick == 16 or tick == 24 then
        screen:Step("QualityLevel", 1)
    elseif tick == 32 then
        screen.Gui.Enabled = true
    elseif tick == 40 then
        screen:ShowPage("Display")
    end
end)
