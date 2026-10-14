--!strict
--[[
	The probe scene for ADR 0128: a grid, pages, flex, a scale, a shape, a size
	limit, a canvas group and the look of a selection.

	Still, flat and at known pixels, so `run_ui_layouts_gate.cmake` can ask
	`imgprobe` what colour each place is -- each claim a point, never a golden
	of the whole frame. Rendered at 1000 by 500.
]]

local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local Background = Color3.fromRGB(20, 20, 30)
local Grey = Color3.fromRGB(60, 60, 60)
local Yellow = Color3.fromRGB(255, 220, 0)
local White = Color3.new(1, 1, 1)
local Red = Color3.new(1, 0, 0)
local Green = Color3.new(0, 1, 0)
local Blue = Color3.new(0, 0, 1)

local backdrop = Instance.new("Frame")
backdrop.Size = UDim2.fromScale(1, 1)
backdrop.BackgroundColor = Background
backdrop.Parent = screen

local function box(parent: Instance, x: number, y: number, width: number, height: number, color: Color3): Frame
    local frame = Instance.new("Frame")
    frame.Position = UDim2.fromOffset(x, y)
    frame.Size = UDim2.fromOffset(width, height)
    frame.BackgroundColor = color
    frame.Parent = parent
    return frame
end

-- 1. A grid: four red cells of 40, 10 apart, in a grey holder 150 wide at
--    (40, 40). Three fit a row -- x 40, 90, 140 -- and the fourth starts the
--    next, at (40, 90).
do
    local holder = box(backdrop, 40, 40, 150, 110, Grey)
    local grid = Instance.new("UIGridLayout")
    grid.CellSize = UDim2.fromOffset(40, 40)
    grid.CellPadding = UDim2.fromOffset(10, 10)
    grid.Parent = holder
    for _ = 1, 4 do
        box(holder, 0, 0, 5, 5, Red)
    end
end

-- 2. Pages: red, green and blue, each the size of a holder at (240, 40), 100
--    by 60, that clips. Turned to the second: green, and nothing beside it.
do
    local holder = box(backdrop, 240, 40, 100, 60, Grey)
    holder.ClipsDescendants = true
    local pages = Instance.new("UIPageLayout")
    pages.Animated = false
    pages.Parent = holder
    for _, color in { Red, Green, Blue } do
        local page = box(holder, 0, 0, 0, 0, color)
        page.Size = UDim2.fromScale(1, 1)
    end
    pages:JumpToIndex(1)
end

-- 3. Flex: three white children 20 wide in a grey holder 200 wide at (400, 40),
--    `SpaceBetween` -- x 400..420, 490..510, 580..600.
do
    local holder = box(backdrop, 400, 40, 200, 30, Grey)
    local list = Instance.new("UIListLayout")
    list.FillDirection = Enum.FillDirection.Horizontal
    list.HorizontalFlex = Enum.UIFlexAlignment.SpaceBetween
    list.Parent = holder
    for _ = 1, 3 do
        box(holder, 0, 0, 20, 30, White)
    end
end

-- 4. A scale: a yellow box 40 by 40 at (640, 40), twice the size -- x 640..720,
--    y 40..120.
do
    local scaled = box(backdrop, 640, 40, 40, 40, Yellow)
    local scale = Instance.new("UIScale")
    scale.Scale = 2
    scale.Parent = scaled
end

-- 5. A shape: a green child as large as its grey holder (120 by 60, at
--    (760, 40)), kept square -- 60 by 60.
do
    local holder = box(backdrop, 760, 40, 120, 60, Grey)
    local square = box(holder, 0, 0, 0, 0, Green)
    square.Size = UDim2.fromScale(1, 1)
    local shape = Instance.new("UIAspectRatioConstraint")
    shape.Parent = square
end

-- 6. A size limit: a white box that asks for 200 by 200 at (40, 200) and may be
--    50 by 30.
do
    local limited = box(backdrop, 40, 200, 200, 200, White)
    local limit = Instance.new("UISizeConstraint")
    limit.MaxSize = Vector2.new(50, 30)
    limit.Parent = limited
end

-- 7. A canvas group at half strength over white, at (240, 200): a red child
--    x 240..340 and a blue one x 300..400, overlapping x 300..340. As one
--    picture the overlap is blue alone, faded -- the colour of the reference
--    at (640, 200), a blue frame at half over white. The control at
--    (440, 200) is the same two frames each faded by itself, where red shows
--    through the blue.
do
    box(backdrop, 240, 200, 160, 80, White)
    local group = Instance.new("CanvasGroup")
    group.Position = UDim2.fromOffset(240, 200)
    group.Size = UDim2.fromOffset(160, 80)
    group.BackgroundTransparency = 1
    group.GroupTransparency = 0.5
    group.Parent = backdrop
    box(group, 0, 0, 100, 80, Red)
    box(group, 60, 0, 100, 80, Blue)

    box(backdrop, 440, 200, 160, 80, White)
    box(backdrop, 440, 200, 100, 80, Red).BackgroundTransparency = 0.5
    box(backdrop, 500, 200, 100, 80, Blue).BackgroundTransparency = 0.5

    box(backdrop, 640, 200, 100, 80, White)
    box(backdrop, 640, 200, 100, 80, Blue).BackgroundTransparency = 0.5
end

-- 8. The selected object's outline: a grey button at (40, 340), 100 by 40,
--    selected. The outline is from 2 to 5 pixels outside it.
do
    local button = Instance.new("TextButton")
    button.Text = ""
    button.Position = UDim2.fromOffset(40, 340)
    button.Size = UDim2.fromOffset(100, 40)
    button.BackgroundColor = Grey
    button.Parent = backdrop
    UIService.SelectedObject = button
end
