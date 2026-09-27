--!strict
--[[
	The security office (ADR 0107): six cameras around a small building, a wall
	of monitors showing all six, and a tablet that shows one of them full size.

	**Every feed is a `CameraTexture`**: a `Camera` placed in a room, drawing
	into `view://cam1` to `view://cam6`, and every picture of it an ordinary
	`ImageLabel` whose `Image` is that name -- on the monitor wall's
	`SurfaceGui`, and on the tablet. Nothing here draws a picture by hand.

	**What it costs is a setting.** The wall's feeds redraw every other frame
	(`UpdateInterval = 2`), and the one on the tablet every frame at a larger
	size -- six views a frame would be six times the world, and this is about
	two and a half. `[render] max_views_per_frame` in `project.toml` is the
	ceiling, and the F3 overlay lists each view and its cost.

	**What the guard carries** is three `ViewportFrame`s in the corner: a
	keycard, a flashlight and a battery turning in their slots.

	Keys: 1 to 6 choose the feed on the tablet, Tab shows or hides the tablet.
]]

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

-- Night: the rooms lit from inside, which is when anybody watches these.
Lighting.ClockTime = 21.5

-- --- The building --------------------------------------------------------------

local function block(name: string, position: vector, size: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Anchored = true
    part.Size = size
    part.CFrame = CFrame.new(position.x, position.y, position.z)
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

local Wall = Color3.fromRGB(150, 152, 160)
block("Floor", vector.create(0, -0.5, 0), vector.create(80, 1, 40), Color3.fromRGB(70, 72, 78))

-- Six rooms in two rows along a corridor, each with something of its own
-- colour turning in the middle, so every feed is recognisably a room and
-- visibly live.
type Room = {
    Name: string,
    Centre: vector,
    Color: Color3,
    Prop: Part,
}

local RoomNames = { "Lobby", "Storage", "Server room", "Loading bay", "Hallway", "Roof access" }
local RoomColors = {
    Color3.fromRGB(230, 70, 60),
    Color3.fromRGB(240, 170, 40),
    Color3.fromRGB(70, 200, 110),
    Color3.fromRGB(60, 150, 235),
    Color3.fromRGB(170, 90, 220),
    Color3.fromRGB(235, 235, 235),
}

local Rooms: { Room } = {}
for index = 1, 6 do
    local column = (index - 1) % 3
    local row = if index <= 3 then -1 else 1
    local centre = vector.create(-24 + column * 24, 0, row * 12)
    -- Three walls; the side facing the corridor is open.
    block("Back", centre + vector.create(0, 3, row * 7.5), vector.create(20, 6, 0.5), Wall)
    block("Left", centre + vector.create(-10, 3, row * 3.75), vector.create(0.5, 6, 7.5), Wall)
    block("Right", centre + vector.create(10, 3, row * 3.75), vector.create(0.5, 6, 7.5), Wall)
    local prop = block("Prop", centre + vector.create(0, 1.5, row * 3), vector.create(2, 3, 2), RoomColors[index])
    -- A ceiling, and one lamp under it: a room lit from inside, as a camera in
    -- a building sees one.
    block("Ceiling", centre + vector.create(0, 6.25, row * 3.75), vector.create(20.5, 0.5, 8), Wall)
    local lamp = Instance.new("PointLight")
    lamp.Color = Color3.fromRGB(255, 236, 200)
    lamp.Brightness = 3
    lamp.Range = 14
    lamp.Parent = prop
    table.insert(Rooms, { Name = RoomNames[index], Centre = centre, Color = RoomColors[index], Prop = prop })
end

-- --- The cameras and their feeds ------------------------------------------------------

type Feed = {
    Room: Room,
    Texture: CameraTexture,
}

local Feeds: { Feed } = {}
for index, room in Rooms do
    local row = if index <= 3 then -1 else 1
    -- High in a front corner of the room, looking at the prop.
    local lens = Instance.new("Camera")
    lens.Name = `Camera{index}`
    lens.FieldOfView = 80
    local mount = room.Centre + vector.create(-8, 5, -row * 0.5)
    lens.CFrame = CFrame.lookAt(mount, room.Prop.Position)
    lens.Parent = workspace

    local texture = Instance.new("CameraTexture")
    texture.Name = `Feed{index}`
    texture.Camera = lens
    texture.ViewName = `cam{index}`
    texture.Resolution = Vector2.new(320, 180)
    texture.UpdateInterval = 2
    texture.Parent = lens
    table.insert(Feeds, { Room = room, Texture = texture })
end

-- --- The office and its monitor wall ------------------------------------------------

local office = vector.create(0, 0, 32)
block("Office floor", office + vector.create(0, -0.5, 0), vector.create(24, 1, 16), Color3.fromRGB(40, 42, 48))
local panel =
    block("Monitor wall", office + vector.create(0, 3.5, -6), vector.create(14, 6, 0.4), Color3.fromRGB(18, 18, 22))
panel.CFrame = CFrame.lookAt(panel.Position, panel.Position + vector.create(0, 0, 1))

local wall = Instance.new("SurfaceGui")
wall.Face = Enum.Face.Front
wall.Brightness = 1.2
wall.Parent = panel

-- A three-by-two grid on the 700 by 300 canvas the 14 by 6 metre face makes.
for index, feed in Feeds do
    local column = (index - 1) % 3
    local row = if index <= 3 then 0 else 1
    local screen = Instance.new("ImageLabel")
    screen.Position = UDim2.fromOffset(12 + column * 228, 10 + row * 145)
    screen.Size = UDim2.fromOffset(216, 122)
    screen.Image = `view://{feed.Texture.ViewName}`
    screen.Parent = wall

    local caption = Instance.new("TextLabel")
    caption.Position = UDim2.fromOffset(6, 100)
    caption.Size = UDim2.fromOffset(200, 18)
    caption.BackgroundTransparency = 1
    caption.TextColor = Color3.fromRGB(230, 255, 230)
    caption.TextSize = 14
    caption.TextXAlignment = Enum.HorizontalAlignment.Left
    caption.Text = `CAM {index}  {feed.Room.Name}`
    caption.Parent = screen
end

local seat = Instance.new("Camera")
seat.Name = "Office"
seat.FieldOfView = 60
seat.CFrame = CFrame.lookAt(office + vector.create(0, 3.2, 6), office + vector.create(0, 3.3, -6))
seat.Parent = workspace
workspace.CurrentCamera = seat

-- --- The tablet -------------------------------------------------------------

local hud = Instance.new("ScreenGui")
hud.Parent = UIService

local tablet = Instance.new("Frame")
tablet.AnchorPoint = Vector2.new(1, 1)
tablet.Position = UDim2.new(1, -24, 1, -24)
tablet.Size = UDim2.fromOffset(520, 330)
tablet.BackgroundColor = Color3.fromRGB(14, 16, 20)
tablet.Parent = hud

local tabletCorner = Instance.new("UICorner")
tabletCorner.CornerRadius = UDim.new(0, 14)
tabletCorner.Parent = tablet

local tabletFeed = Instance.new("ImageLabel")
tabletFeed.Position = UDim2.fromOffset(16, 16)
tabletFeed.Size = UDim2.fromOffset(488, 274)
tabletFeed.Parent = tablet

local tabletCaption = Instance.new("TextLabel")
tabletCaption.Position = UDim2.fromOffset(16, 296)
tabletCaption.Size = UDim2.fromOffset(488, 24)
tabletCaption.BackgroundTransparency = 1
tabletCaption.TextColor = Color3.fromRGB(200, 230, 200)
tabletCaption.TextSize = 18
tabletCaption.TextXAlignment = Enum.HorizontalAlignment.Left
tabletCaption.Parent = tablet

-- **The chosen feed draws every frame, and larger**: the tablet is what the
-- player is looking at. The rest go back to every other frame at the wall's
-- size.
local Chosen = 1
local function choose(index: number)
    for other, feed in Feeds do
        local chosen = other == index
        feed.Texture.UpdateInterval = if chosen then 1 else 2
        feed.Texture.Resolution = if chosen then Vector2.new(640, 360) else Vector2.new(320, 180)
    end
    Chosen = index
    tabletFeed.Image = `view://cam{index}`
    tabletCaption.Text = `CAM {index}  {Feeds[index].Room.Name}    1-6 switch, Tab hides`
end
choose(1)

-- --- What the guard carries ----------------------------------------------------

-- **Three items turning in slots**: each slot a `ViewportFrame` with the item
-- inside it and no background, framed automatically. What is inside a frame is
-- drawn there and nowhere else, and nothing simulates it -- the script turns
-- each one by setting its `CFrame`.
type Item = {
    Label: string,
    Model: Part,
}

local function slot(index: number, label: string, shape: EnumPartShape, size: vector, color: Color3): Item
    local frame = Instance.new("ViewportFrame")
    frame.Position = UDim2.fromOffset(24 + (index - 1) * 104, 24)
    frame.Size = UDim2.fromOffset(92, 92)
    frame.BackgroundColor = Color3.fromRGB(14, 16, 20)
    frame.BackgroundTransparency = 0.2
    frame.Parent = hud
    local rounded = Instance.new("UICorner")
    rounded.CornerRadius = UDim.new(0, 10)
    rounded.Parent = frame

    local model = Instance.new("Part")
    model.Shape = shape
    model.Size = size
    model:SetMaterialParameter("Color", color)
    model.Parent = frame

    local caption = Instance.new("TextLabel")
    caption.Position = UDim2.fromOffset(0, 72)
    caption.Size = UDim2.fromOffset(92, 18)
    caption.BackgroundTransparency = 1
    caption.TextColor = Color3.fromRGB(210, 214, 222)
    caption.TextSize = 13
    caption.Text = label
    caption.Parent = frame
    return { Label = label, Model = model }
end

local Items: { Item } = {
    slot(1, "Keycard", Enum.PartShape.Block, vector.create(1.6, 1, 0.08), Color3.fromRGB(230, 70, 60)),
    slot(2, "Flashlight", Enum.PartShape.Cylinder, vector.create(2, 0.5, 0.5), Color3.fromRGB(60, 60, 70)),
    slot(3, "Battery", Enum.PartShape.Cylinder, vector.create(1, 0.4, 0.4), Color3.fromRGB(240, 170, 40)),
}

-- --- Keys ---------------------------------------------------------------------

local Controls = Instance.new("InputContext")
Controls.Name = "Monitors"
Controls.Parent = workspace

local Choices: { InputAction } = {}
local Digits = {
    Enum.KeyCode.Digit1,
    Enum.KeyCode.Digit2,
    Enum.KeyCode.Digit3,
    Enum.KeyCode.Digit4,
    Enum.KeyCode.Digit5,
    Enum.KeyCode.Digit6,
}
for index, key in Digits do
    local action = Instance.new("InputAction")
    action.Name = `Camera{index}`
    action.Parent = Controls
    local binding = Instance.new("InputBinding")
    binding.KeyCode = key
    binding.Parent = action
    table.insert(Choices, action)
end

local ToggleAction = Instance.new("InputAction")
ToggleAction.Name = "Tablet"
ToggleAction.Parent = Controls
local ToggleKey = Instance.new("InputBinding")
ToggleKey.KeyCode = Enum.KeyCode.Tab
ToggleKey.Parent = ToggleAction

local TogglePressed = false
local Clock = 0

RunService.Heartbeat:Connect(function(dt: number)
    for index, action in Choices do
        if action:GetState() :: boolean and index ~= Chosen then
            choose(index)
        end
    end
    local pressed = ToggleAction:GetState() :: boolean
    if pressed and not TogglePressed then
        tablet.Visible = not tablet.Visible
    end
    TogglePressed = pressed

    -- Every room's prop turns, so every feed is visibly live.
    Clock += dt
    for index, item in Items do
        item.Model.CFrame = CFrame.fromEuler(0.35, Clock * 0.9 + index, 0)
    end
    for index, room in Rooms do
        local position = room.Prop.Position
        room.Prop.CFrame = CFrame.new(position.x, position.y, position.z)
            * CFrame.fromEuler(0, Clock * (0.6 + index * 0.15), 0)
    end
end)
