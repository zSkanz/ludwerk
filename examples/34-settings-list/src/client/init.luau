--!strict
--[[
	A settings list as a phone has one (G40): a long column of rows in a
	`ScrollFrame` that a finger throws, that gives at either end and springs
	back, with a strip of wallpapers inside it that scrolls sideways -- and not
	one line of scrolling code.

	What does each job:

	- `AutomaticCanvasSize = Y` keeps the canvas as tall as the rows, however
	  many there are;
	- `ScrollingDirection` says the column goes down and the strip across, so a
	  drag that starts down scrolls the column and one that starts sideways the
	  strip;
	- `ElasticBehavior` is left at `WhenScrollable`: the column gives at its
	  ends; a list too short to scroll would not;
	- every row is a button, so the arrows or a d-pad walk down the list and the
	  column scrolls to keep the one selected in sight;
	- the title writes `CanvasPosition`, which is how a script scrolls.
]]

local UIService = game:GetService("UIService")

local Ink = Color3.fromRGB(14, 15, 20)
local Row = Color3.fromRGB(30, 32, 42)
local Accent = Color3.fromRGB(64, 196, 120)
local Off = Color3.fromRGB(70, 74, 90)
local Text = Color3.fromRGB(232, 234, 240)
local Dim = Color3.fromRGB(146, 152, 170)

local screen = Instance.new("ScreenGui")
screen.Name = "Settings"
-- Laid out as if the window were 960 tall: a phone held upright.
screen.ReferenceHeight = 960
screen.Parent = UIService

local backdrop = Instance.new("Frame")
backdrop.Size = UDim2.fromScale(1, 1)
backdrop.BackgroundColor = Ink
backdrop.Parent = screen

-- One column, never wider than a phone is, in the middle of a wider window.
local phone = Instance.new("Frame")
phone.AnchorPoint = Vector2.new(0.5, 0)
phone.Position = UDim2.fromScale(0.5, 0)
phone.Size = UDim2.fromScale(1, 1)
phone.BackgroundTransparency = 1
phone.Parent = backdrop

local widest = Instance.new("UISizeConstraint")
widest.MaxSize = Vector2.new(560, 100000)
widest.Parent = phone

local function label(parent: Instance, text: string, size: number, color: Color3): TextLabel
    local made = Instance.new("TextLabel")
    made.BackgroundTransparency = 1
    made.Text = text
    made.TextSize = size
    made.TextColor = color
    made.TextXAlignment = Enum.HorizontalAlignment.Left
    made.Parent = parent
    return made
end

local function corner(parent: Instance, radius: number)
    local rounded = Instance.new("UICorner")
    rounded.CornerRadius = UDim.new(0, radius)
    rounded.Parent = parent
end

-- --- The title, which scrolls the list back to the top ---------------------------

local title = Instance.new("TextButton")
title.Size = UDim2.new(1, 0, 0, 72)
title.BackgroundColor = Ink
title.Text = "Settings"
title.TextSize = 32
title.TextColor = Text
title.Parent = phone

-- --- The list --------------------------------------------------------------------

local list = Instance.new("ScrollFrame")
list.Position = UDim2.fromOffset(0, 72)
list.Size = UDim2.new(1, 0, 1, -72)
list.BackgroundTransparency = 1
list.ScrollBarThickness = 4
list.ScrollingDirection = Enum.ScrollingDirection.Y
list.AutomaticCanvasSize = Enum.AutomaticSize.Y
list.Parent = phone

local column = Instance.new("UIListLayout")
column.Padding = UDim.new(0, 2)
column.Parent = list

title.Activated:Connect(function()
    list.CanvasPosition = Vector2.zero
end)

local order = 0
local function nextOrder(): number
    order += 1
    return order
end

local function heading(text: string)
    local made = label(list, text, 18, Dim)
    made.LayoutOrder = nextOrder()
    made.Size = UDim2.new(1, 0, 0, 48)
    local inset = Instance.new("UIPadding")
    inset.PaddingLeft = UDim.new(0, 20)
    inset.Parent = made
end

-- A row with a switch on its right: pressed, it turns over.
local function toggle(text: string, on: boolean)
    local row = Instance.new("TextButton")
    row.LayoutOrder = nextOrder()
    row.Size = UDim2.new(1, 0, 0, 64)
    row.BackgroundColor = Row
    row.Text = ""
    row.Parent = list

    local name = label(row, text, 22, Text)
    name.Position = UDim2.fromOffset(20, 0)
    name.Size = UDim2.new(1, -120, 1, 0)

    local switch = Instance.new("Frame")
    switch.AnchorPoint = Vector2.new(1, 0.5)
    switch.Position = UDim2.new(1, -20, 0.5, 0)
    switch.Size = UDim2.fromOffset(56, 32)
    corner(switch, 16)
    switch.Parent = row

    local knob = Instance.new("Frame")
    knob.AnchorPoint = Vector2.new(0, 0.5)
    knob.Size = UDim2.fromOffset(26, 26)
    knob.BackgroundColor = Text
    corner(knob, 13)
    knob.Parent = switch

    local function show()
        switch.BackgroundColor = if on then Accent else Off
        knob.Position = if on then UDim2.new(1, -29, 0.5, 0) else UDim2.new(0, 3, 0.5, 0)
    end
    show()
    row.Activated:Connect(function()
        on = not on
        show()
    end)
end

-- --- Display ---------------------------------------------------------------------

heading("DISPLAY")
toggle("Dark theme", true)
toggle("Auto-rotate", false)
toggle("Larger text", false)

-- --- Wallpaper: a strip that scrolls across, inside the list -----------------------

heading("WALLPAPER")

local strip = Instance.new("ScrollFrame")
strip.LayoutOrder = nextOrder()
strip.Size = UDim2.new(1, 0, 0, 180)
strip.BackgroundTransparency = 1
strip.ScrollBarThickness = 0
strip.ScrollingDirection = Enum.ScrollingDirection.X
strip.AutomaticCanvasSize = Enum.AutomaticSize.X
strip.Parent = list

local stripPad = Instance.new("UIPadding")
stripPad.PaddingLeft = UDim.new(0, 16)
stripPad.PaddingRight = UDim.new(0, 16)
stripPad.Parent = strip

local across = Instance.new("UIListLayout")
across.FillDirection = Enum.FillDirection.Horizontal
across.Padding = UDim.new(0, 12)
across.VerticalAlignment = Enum.VerticalAlignment.Center
across.Parent = strip

local Wallpapers = {
    Color3.fromRGB(232, 98, 82),
    Color3.fromRGB(246, 176, 66),
    Color3.fromRGB(96, 196, 120),
    Color3.fromRGB(70, 160, 232),
    Color3.fromRGB(150, 110, 232),
    Color3.fromRGB(232, 110, 180),
    Color3.fromRGB(90, 200, 200),
    Color3.fromRGB(200, 200, 90),
}
for index, color in Wallpapers do
    local card = Instance.new("TextButton")
    card.LayoutOrder = index
    card.Size = UDim2.fromOffset(110, 160)
    card.BackgroundColor = color
    card.Text = ""
    corner(card, 12)
    card.Parent = strip
    card.Activated:Connect(function()
        backdrop.BackgroundColor = color:Lerp(Ink, 0.85)
    end)
end

-- --- Notifications: long enough to throw ------------------------------------------

heading("NOTIFICATIONS")
local Apps = {
    "Calendar",
    "Camera",
    "Clock",
    "Contacts",
    "Files",
    "Gallery",
    "Mail",
    "Maps",
    "Messages",
    "Music",
    "News",
    "Notes",
    "Phone",
    "Podcasts",
    "Reminders",
    "Store",
    "Translate",
    "Videos",
    "Wallet",
    "Weather",
}
for index, app in Apps do
    toggle(app, index % 3 ~= 0)
end

heading("ABOUT")
local about = label(list, "Ludwerk settings example", 18, Dim)
about.LayoutOrder = nextOrder()
about.Size = UDim2.new(1, 0, 0, 96)
about.TextXAlignment = Enum.HorizontalAlignment.Center
