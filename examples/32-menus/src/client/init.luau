--!strict
--[[
	Menus (ADR 0128): an inventory in a grid, a tutorial in pages, and an
	options page with a slider -- one window that fits any screen and is driven
	by a mouse, a finger, a gamepad or the arrow keys, with no layout code.

	What does each job:

	- the window is a `CanvasGroup`, so it fades in as one picture, and a
	  `UIScale` pops it open;
	- a `UISizeConstraint` keeps it between two sizes whatever the window is;
	- the three tabs share their bar by flex;
	- the body is a `UIPageLayout`: a tab, a swipe, the wheel or a shoulder
	  button turns the page;
	- the inventory is a `UIGridLayout` in a `ScrollFrame`;
	- the tutorial is a second page layout, going round;
	- the slider's thumb has a `UIDragDetector` bound to its track;
	- and every button is selectable, so the d-pad, the stick and the arrows
	  move between them and `ButtonA` or Enter presses one.
]]

local TweenService = game:GetService("TweenService")
local UIService = game:GetService("UIService")

local Ink = Color3.fromRGB(18, 20, 28)
local Panel = Color3.fromRGB(30, 34, 46)
local Raised = Color3.fromRGB(44, 50, 68)
local Accent = Color3.fromRGB(86, 156, 255)
local Text = Color3.fromRGB(226, 230, 240)
local Dim = Color3.fromRGB(150, 158, 178)

local screen = Instance.new("ScreenGui")
screen.Name = "Menus"
-- Laid out as if the window were 720 tall, whatever it is: a phone and a
-- monitor show the same menu.
screen.ReferenceHeight = 720
screen.Parent = UIService

local backdrop = Instance.new("Frame")
backdrop.Size = UDim2.fromScale(1, 1)
backdrop.BackgroundColor = Ink
backdrop.Parent = screen

local function corner(parent: Instance, radius: number)
    local rounded = Instance.new("UICorner")
    rounded.CornerRadius = UDim.new(0, radius)
    rounded.Parent = parent
end

local function padding(parent: Instance, amount: number)
    local pad = Instance.new("UIPadding")
    pad.PaddingTop = UDim.new(0, amount)
    pad.PaddingBottom = UDim.new(0, amount)
    pad.PaddingLeft = UDim.new(0, amount)
    pad.PaddingRight = UDim.new(0, amount)
    pad.Parent = parent
end

local function label(parent: Instance, text: string, size: number, color: Color3): TextLabel
    local made = Instance.new("TextLabel")
    made.BackgroundTransparency = 1
    made.Text = text
    made.TextSize = size
    made.TextColor = color
    made.Parent = parent
    return made
end

local function button(parent: Instance, text: string): TextButton
    local made = Instance.new("TextButton")
    made.Text = text
    made.TextSize = 20
    made.TextColor = Text
    made.BackgroundColor = Raised
    corner(made, 8)
    made.Parent = parent
    return made
end

-- --- The window -----------------------------------------------------------------

local window = Instance.new("CanvasGroup")
window.Name = "Window"
window.AnchorPoint = Vector2.new(0.5, 0.5)
window.Position = UDim2.fromScale(0.5, 0.5)
window.Size = UDim2.fromScale(0.82, 0.84)
window.BackgroundColor = Panel
corner(window, 14)
padding(window, 18)
window.Parent = backdrop

-- Four fifths of the screen, and never smaller than can be used nor larger
-- than is comfortable to read across.
local limits = Instance.new("UISizeConstraint")
limits.MinSize = Vector2.new(420, 360)
limits.MaxSize = Vector2.new(1040, 640)
limits.Parent = window

local column = Instance.new("UIListLayout")
column.Padding = UDim.new(0, 14)
column.Parent = window

-- --- The tabs -------------------------------------------------------------------

local tabs = Instance.new("Frame")
tabs.Name = "Tabs"
tabs.LayoutOrder = 1
tabs.Size = UDim2.new(1, 0, 0, 48)
tabs.BackgroundTransparency = 1
tabs.Parent = window

-- The three share the bar: each grows by the same amount until it is full.
local tabRow = Instance.new("UIListLayout")
tabRow.FillDirection = Enum.FillDirection.Horizontal
tabRow.Padding = UDim.new(0, 10)
tabRow.HorizontalFlex = Enum.UIFlexAlignment.Fill
tabRow.ItemLineAlignment = Enum.ItemLineAlignment.Stretch
tabRow.Parent = tabs

-- --- The body: three pages ------------------------------------------------------

local body = Instance.new("Frame")
body.Name = "Body"
body.LayoutOrder = 2
-- What the tabs and the hint leave: the window's height less theirs and the
-- two gaps between the three.
body.Size = UDim2.new(1, 0, 1, -(48 + 24 + 14 * 2))
body.BackgroundTransparency = 1
body.ClipsDescendants = true
body.Parent = window

local pages = Instance.new("UIPageLayout")
pages.TweenTime = 0.25
pages.Parent = body

local function page(name: string, order: number): Frame
    local made = Instance.new("Frame")
    made.Name = name
    made.LayoutOrder = order
    made.Size = UDim2.fromScale(1, 1)
    made.BackgroundTransparency = 1
    made.Parent = body
    return made
end

local tabButtons: { TextButton } = {}
local function tab(text: string, index: number)
    local made = button(tabs, text)
    made.LayoutOrder = index
    made.Size = UDim2.new(0, 120, 1, 0)
    made.Activated:Connect(function()
        pages:JumpToIndex(index - 1)
    end)
    table.insert(tabButtons, made)
end

local function showCurrentTab()
    for _, each in tabButtons do
        each.BackgroundColor = Raised
    end
    local current = pages.CurrentPage
    if current ~= nil then
        tabButtons[current.LayoutOrder].BackgroundColor = Accent
    end
end
pages.PageEnter:Connect(showCurrentTab)

-- --- Page 1: the inventory, a grid that scrolls ----------------------------------

local inventory = page("Inventory", 1)
tab("Inventory", 1)

local slots = Instance.new("ScrollFrame")
slots.Size = UDim2.fromScale(1, 1)
slots.BackgroundTransparency = 1
slots.ScrollBarThickness = 8
-- As tall as the grid, however many slots fit across.
slots.AutomaticCanvasSize = Enum.AutomaticSize.Y
slots.Parent = inventory

local grid = Instance.new("UIGridLayout")
grid.CellSize = UDim2.fromOffset(84, 84)
grid.CellPadding = UDim2.fromOffset(10, 10)
grid.HorizontalAlignment = Enum.HorizontalAlignment.Center
grid.Parent = slots

local ItemColors = {
    Color3.fromRGB(214, 96, 96),
    Color3.fromRGB(232, 186, 82),
    Color3.fromRGB(110, 196, 124),
    Color3.fromRGB(98, 162, 230),
    Color3.fromRGB(170, 124, 224),
}

local picked = label(window, "", 16, Dim)
for index = 1, 42 do
    local slot = Instance.new("TextButton")
    slot.Name = `Slot{index}`
    slot.LayoutOrder = index
    slot.BackgroundColor = Raised
    slot.Text = if index % 3 == 0 then "" else tostring(index)
    slot.TextSize = 18
    slot.TextColor = Text
    corner(slot, 10)
    if index % 3 ~= 0 then
        -- An item: a coloured square kept square, whatever the cell is.
        local icon = Instance.new("Frame")
        icon.AnchorPoint = Vector2.new(0.5, 0.5)
        icon.Position = UDim2.fromScale(0.5, 0.5)
        icon.Size = UDim2.fromScale(0.7, 0.5)
        icon.BackgroundColor = ItemColors[index % #ItemColors + 1]
        icon.BackgroundTransparency = 0.25
        corner(icon, 8)
        local square = Instance.new("UIAspectRatioConstraint")
        square.Parent = icon
        icon.Parent = slot
    end
    slot.Activated:Connect(function()
        picked.Text = if index % 3 == 0 then `Slot {index} is empty` else `Picked item {index}`
    end)
    slot.Parent = slots
end

-- --- Page 2: the tutorial, pages inside a page ----------------------------------

local tutorial = page("Tutorial", 2)
tab("How to play", 2)

local cards = Instance.new("Frame")
cards.Size = UDim2.new(1, 0, 1, -60)
cards.BackgroundTransparency = 1
cards.ClipsDescendants = true
cards.Parent = tutorial

local steps = Instance.new("UIPageLayout")
steps.Circular = true
steps.Padding = UDim.new(0, 24)
-- The shoulder buttons turn the window's pages, not these.
steps.GamepadInputEnabled = false
steps.Parent = cards

local Steps = {
    { "Move", "The arrows, the d-pad or the left stick move the selection; a mouse or a finger just presses." },
    { "Turn the page", "A tab, a swipe, the wheel or a shoulder button turns to the next page." },
    { "Drag", "The slider on the last page is dragged by its thumb and stays on its track." },
}
for index, step in Steps do
    local card = Instance.new("Frame")
    card.LayoutOrder = index
    card.Size = UDim2.fromScale(0.8, 0.9)
    card.BackgroundColor = Raised
    corner(card, 12)
    padding(card, 20)
    card.Parent = cards

    local title = label(card, step[1], 40, Text)
    title.Size = UDim2.new(1, 0, 0.4, 0)
    -- As large as its box lets it be, between two sizes: readable on a phone
    -- and not a billboard on a monitor.
    title.TextScaled = true
    local titleLimits = Instance.new("UITextSizeConstraint")
    titleLimits.MinTextSize = 22
    titleLimits.MaxTextSize = 54
    titleLimits.Parent = title

    local words = label(card, step[2], 20, Dim)
    words.Position = UDim2.fromScale(0, 0.45)
    words.Size = UDim2.fromScale(1, 0.55)
    words.TextWrapped = true
    words.TextYAlignment = Enum.VerticalAlignment.Top
end

local stepBar = Instance.new("Frame")
stepBar.AnchorPoint = Vector2.new(0, 1)
stepBar.Position = UDim2.fromScale(0, 1)
stepBar.Size = UDim2.new(1, 0, 0, 46)
stepBar.BackgroundTransparency = 1
stepBar.Parent = tutorial

-- The two buttons at the ends and the counter in the middle: the room that is
-- left goes between them.
local stepRow = Instance.new("UIListLayout")
stepRow.FillDirection = Enum.FillDirection.Horizontal
stepRow.HorizontalFlex = Enum.UIFlexAlignment.SpaceBetween
stepRow.ItemLineAlignment = Enum.ItemLineAlignment.Stretch
stepRow.Parent = stepBar

local back = button(stepBar, "Back")
back.LayoutOrder = 1
back.Size = UDim2.new(0, 140, 1, 0)
back.Activated:Connect(function()
    steps:Previous()
end)

local counter = label(stepBar, `1 / {#Steps}`, 18, Dim)
counter.LayoutOrder = 2
counter.Size = UDim2.new(0, 120, 1, 0)

local nextStep = button(stepBar, "Next")
nextStep.LayoutOrder = 3
nextStep.Size = UDim2.new(0, 140, 1, 0)
nextStep.Activated:Connect(function()
    steps:Next()
end)

steps.PageEnter:Connect(function(card)
    counter.Text = `{card.LayoutOrder} / {#Steps}`
end)

-- --- Page 3: options, a slider and a column of buttons ---------------------------

local options = page("Options", 3)
tab("Options", 3)

local optionColumn = Instance.new("UIListLayout")
optionColumn.Padding = UDim.new(0, 16)
optionColumn.HorizontalAlignment = Enum.HorizontalAlignment.Center
optionColumn.Parent = options

local volume = label(options, "Volume 50%", 22, Text)
volume.LayoutOrder = 1
volume.Size = UDim2.new(0, 420, 0, 32)

local track = Instance.new("Frame")
track.LayoutOrder = 2
track.Size = UDim2.new(0, 420, 0, 28)
track.BackgroundColor = Raised
corner(track, 14)
track.Parent = options

local thumb = Instance.new("TextButton")
thumb.Text = ""
thumb.Size = UDim2.new(0, 56, 1, 0)
-- Half way along what is left of the track beside it.
thumb.Position = UDim2.fromScale(0.5 * (420 - 56) / 420, 0)
thumb.BackgroundColor = Accent
corner(thumb, 14)
thumb.Parent = track

-- Dragged along the track and kept on it; written as a fraction of the track,
-- so it is in the same place at any size.
local drag = Instance.new("UIDragDetector")
drag.DragStyle = Enum.UIDragDetectorDragStyle.TranslateLine
drag.ResponseStyle = Enum.UIDragDetectorResponseStyle.Scale
drag.BoundingUI = track
drag.Parent = thumb

local function showVolume()
    local along = thumb.Position.X.Scale / ((420 - 56) / 420)
    volume.Text = `Volume {math.round(math.clamp(along, 0, 1) * 100)}%`
end
drag.DragContinue:Connect(showVolume)
drag.DragEnd:Connect(showVolume)

for index, text in { "Sound", "Music", "Vibration" } do
    local toggle = button(options, `{text}: on`)
    toggle.LayoutOrder = 2 + index
    toggle.Size = UDim2.new(0, 420, 0, 44)
    local on = true
    toggle.Activated:Connect(function()
        on = not on
        toggle.Text = `{text}: {if on then "on" else "off"}`
    end)
end

-- --- The hint, and opening ------------------------------------------------------

picked.LayoutOrder = 3
picked.Size = UDim2.new(1, 0, 0, 24)
picked.Text = "Arrows or d-pad: move   Enter or A: press   L1 R1, wheel or swipe: turn the page"
-- One line at any width: as large as fits, and never past what a hint should be.
picked.TextScaled = true
local hintLimits = Instance.new("UITextSizeConstraint")
hintLimits.MinTextSize = 9
hintLimits.MaxTextSize = 16
hintLimits.Parent = picked

pages:JumpToIndex(0)
showCurrentTab()
-- A menu that opens selects its first button, so a gamepad has somewhere to
-- start; a press of the mouse puts the outline away again.
UIService.SelectedObject = tabButtons[1]

-- Opening: the whole window fades in as one and pops to its size.
local pop = Instance.new("UIScale")
pop.Scale = 0.92
pop.Parent = window
window.GroupTransparency = 1
local opening = TweenInfo.new(0.35, Enum.EasingStyle.Back, Enum.EasingDirection.Out)
TweenService:Create(window, opening, { GroupTransparency = 0 }):Play()
TweenService:Create(pop, opening, { Scale = 1 }):Play()
