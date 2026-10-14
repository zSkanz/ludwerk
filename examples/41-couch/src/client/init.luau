--!strict
--[[
	Several players at one screen (ADR 0205): two to four at one machine, seen
	from above, each with a view of the window.

	What does each job:

	- **A view a player.** A local player with a `Camera` has a part of the
	  window, and `workspace.ScreenSplit` says how the window is shared:
	  `Columns`, `Rows`, `Grid` -- or `Dynamic`, one picture while the players
	  are together and their own views when they walk apart.
	- **`@engine/splitscreen`** drives `Dynamic`: it frames everybody with one
	  camera, and as two players part it turns a line across the direction
	  between them and holds each in the middle of their own side. Three and
	  four get the window's quarters, faded in.
	- **An interface each.** Every seat has a `ScreenGui` with its `Player`:
	  it is laid out in that player's view and navigated by that player's
	  controller. The line at the top belongs to nobody, and is the whole
	  window's.
	- **Who is who** is an `InputContext` a seat, each reading one controller.
	  The first seat also reads W A S D and the second the arrow keys, so two
	  can try it at one keyboard.

	Tab or Y changes how the window is shared. Enter seats a second player at
	the keyboard; a controller seats itself by being plugged in.

	`--scene-data` puts it in a state for a picture, as the gates do:
	`{"players": 4, "mode": "Grid", "apart": 30}` seats four, thirty metres
	from the middle, and stands them still.
]]

local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

local splitscreen = require("@engine/splitscreen")

Lighting.ClockTime = 15.5
-- Seen from above, across a hundred metres of ground: no haze between the
-- camera and it.
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- --- The ground ----------------------------------------------------------------

local Half = 60
local Text = Color3.fromRGB(240, 240, 235)

local function block(name: string, size: vector, position: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Anchored = true
    part.Parent = workspace
    return part
end

block("Floor", vector.create(Half * 2, 1, Half * 2), vector.create(0, -0.5, 0), Color3.fromRGB(88, 104, 92))

-- Landmarks, so one part of the ground does not look like another: a tower at
-- every twelfth metre, its height and colour from where it stands. Nothing
-- random -- the same field on every run, which is what a picture is held to.
local Tones = {
    Color3.fromRGB(196, 96, 72),
    Color3.fromRGB(212, 172, 84),
    Color3.fromRGB(86, 142, 176),
    Color3.fromRGB(136, 112, 168),
    Color3.fromRGB(96, 156, 110),
}
-- `"dense": true` in the scene data stands one every fourth metre instead: a
-- thousand of them, which is what the cost of a view is measured over.
type Demo = { players: number?, mode: string?, apart: number?, turn: number?, dense: boolean?, line: boolean? }
local demo = (SceneService:GetLoadData() or {}) :: Demo
local Pitch = if demo.dense then 4 else 12
local Reach = if demo.dense then 14 else 4
for column = -Reach, Reach do
    for row = -Reach, Reach do
        if not (column == 0 and row == 0) then
            local pick = (column * 7 + row * 13) % #Tones + 1
            local height = 2 + (column * 5 + row * 3) % 7
            local width = if demo.dense then 1.2 else 2.4
            block(
                "Tower",
                vector.create(width, height, width),
                vector.create(column * Pitch + Pitch / 3, height / 2, row * Pitch + Pitch / 3),
                Tones[pick]
            )
        end
    end
end

-- --- The seats -----------------------------------------------------------------

local SeatColors = {
    Color3.fromRGB(236, 92, 80),
    Color3.fromRGB(84, 156, 240),
    Color3.fromRGB(246, 204, 76),
    Color3.fromRGB(110, 208, 128),
}
-- Where each stands at the start: a small square round the middle.
local Starts = {
    vector.create(-2, 1, -2),
    vector.create(2, 1, -2),
    vector.create(-2, 1, 2),
    vector.create(2, 1, 2),
}
local Speed = 14
-- From the point a camera looks at to the camera.
local Offset = vector.create(0, 30, 18)

type Seat = {
    Number: number,
    Player: Player,
    Body: Part,
    -- The seat's own camera, centred on its body: what the fixed layouts use.
    Own: Camera,
    Label: TextLabel,
}
local seats: { Seat } = {}
local screen: splitscreen.Screen? = nil

local function bindKeys(move: InputAction, left: EnumKeyCode, right: EnumKeyCode, up: EnumKeyCode, down: EnumKeyCode)
    local binding = Instance.new("InputBinding")
    binding.Left = left
    binding.Right = right
    binding.Up = up
    binding.Down = down
    binding.Parent = move
end

local function seat(player: Player): Seat
    local number = #seats + 1

    local context = Instance.new("InputContext")
    context.Name = `Seat{number}`
    -- Nil is the first player's, as a game with one player never says whose.
    context.Player = if number == 1 then nil else player
    context.GamepadId = number
    context.Parent = InputService
    local move = Instance.new("InputAction")
    move.Name = "Move"
    move.Type = Enum.InputActionType.Direction2D
    move.Parent = context
    local stick = Instance.new("InputBinding")
    stick.KeyCode = Enum.KeyCode.LeftThumbstick
    stick.Parent = move
    if number == 1 then
        bindKeys(move, Enum.KeyCode.A, Enum.KeyCode.D, Enum.KeyCode.W, Enum.KeyCode.S)
    elseif number == 2 then
        bindKeys(move, Enum.KeyCode.Left, Enum.KeyCode.Right, Enum.KeyCode.Up, Enum.KeyCode.Down)
    end
    -- The controller that moves the body is the one that steps through this
    -- player's interface.
    player.GamepadId = number

    local body = block(`Player{number}`, vector.create(1.4, 2, 1.4), Starts[number], SeatColors[number])
    body.Shape = Enum.PartShape.Capsule

    local own = Instance.new("Camera")
    own.Name = `Camera{number}`
    own.FieldOfView = 50
    own.FarPlane = 600
    own.CFrame = CFrame.lookAt(body.Position + Offset, body.Position)
    own.Parent = workspace

    -- **This player's interface**: laid out in this player's view, wherever
    -- in the window that is.
    local gui = Instance.new("ScreenGui")
    gui.Name = `Hud{number}`
    gui.ReferenceHeight = 720
    gui.Player = player
    gui.Parent = UIService
    -- A corner a seat: its own view's while each has a rectangle, and the
    -- window's while the picture is one or parts across a line -- where an
    -- interface is the whole window's, and four in one corner would cover
    -- each other.
    local across = if number % 2 == 1 then 0 else 1
    local down = if number <= 2 then 0 else 1
    local label = Instance.new("TextLabel")
    label.AnchorPoint = Vector2.new(across, down)
    label.Position = UDim2.new(across, if across == 0 then 16 else -16, down, if down == 0 then 12 else -56)
    label.Size = UDim2.new(0, 220, 0, 30)
    label.BackgroundColor = SeatColors[number]
    label.TextColor = Color3.fromRGB(20, 20, 24)
    label.TextSize = 18
    label.Text = LocalizationService:Translate("hud.player", { number = number })
    label.Parent = gui

    local made: Seat = { Number = number, Player = player, Body = body, Own = own, Label = label }
    table.insert(seats, made)
    return made
end

-- --- How the window is shared ----------------------------------------------------

local Modes: { EnumScreenSplit } = {
    Enum.ScreenSplit.Dynamic,
    Enum.ScreenSplit.Columns,
    Enum.ScreenSplit.Rows,
    Enum.ScreenSplit.Grid,
    Enum.ScreenSplit.None,
}
local mode = 1

local overview = Instance.new("Camera")
overview.Name = "Overview"
overview.FieldOfView = 50
overview.FarPlane = 600
overview.CFrame = CFrame.lookAt(Offset, vector.zero)
overview.Parent = workspace
workspace.CurrentCamera = overview

local hud = Instance.new("ScreenGui")
hud.Name = "Hud"
hud.ReferenceHeight = 720
hud.DisplayOrder = 10
hud.Parent = UIService
local caption = Instance.new("TextLabel")
caption.AnchorPoint = Vector2.new(0.5, 1)
caption.Position = UDim2.new(0.5, 0, 1, -14)
caption.Size = UDim2.new(0, 940, 0, 30)
caption.BackgroundColor = Color3.fromRGB(20, 20, 24)
caption.BackgroundTransparency = 0.25
caption.TextColor = Text
caption.TextSize = 16
caption.Parent = hud

local function say()
    caption.Text = LocalizationService:Translate("hud.mode", {
        mode = LocalizationService:Translate(`mode.{Modes[mode].Name}`),
        players = #seats,
    })
    for _, each in seats do
        each.Label.Text = LocalizationService:Translate("hud.player", { number = each.Number })
    end
end
LocalizationService.LocaleChanged:Connect(say)

local function share()
    local how = Modes[mode]
    if screen ~= nil then
        screen:Destroy()
        screen = nil
    end
    if how == Enum.ScreenSplit.Dynamic then
        -- The module frames them with `overview`, and gives each a camera of
        -- its making.
        local made = splitscreen.follow({ Offset = Offset, SplitDistance = 24 })
        for _, each in seats do
            made:Add(each.Player, each.Body)
        end
        screen = made
    else
        for _, each in seats do
            each.Player.Camera = each.Own
        end
        workspace.ScreenSplit = how
    end
    say()
end

local function join(): Seat?
    if #seats >= 4 then
        return nil
    end
    local player: Player? = if #seats == 0 then NetworkService.LocalPlayer else NetworkService:AddLocalPlayer()
    if player == nil then
        return nil
    end
    local made = seat(player)
    share()
    return made
end

-- --- Running -----------------------------------------------------------------------

join()
for _ = 2, demo.players or 1 do
    join()
end
if demo.mode ~= nil then
    for index, how in Modes do
        if how.Name == demo.mode then
            mode = index
        end
    end
    share()
end
if demo.line then
    -- A line no part of the ground is the colour of, for a picture to be
    -- asked about.
    workspace.SplitLineColor = Color3.new(1, 0, 1)
    workspace.SplitLineThickness = 8
end
if demo.apart ~= nil then
    -- Each seat on its own spoke from the middle, `turn` degrees round.
    for index, each in seats do
        local angle = math.rad((demo.turn or 0) + (index - 1) * 360 / #seats)
        local radius = if #seats == 2 then demo.apart / 2 else demo.apart
        each.Body.Position = vector.create(math.cos(angle) * radius, 1, -math.sin(angle) * radius)
    end
end

-- A controller plugged in is a player seated: the second seat for the second
-- controller, and so on.
InputService.GamepadConnected:Connect(function(id: number)
    while #seats < math.min(id, 4) do
        if join() == nil then
            break
        end
    end
end)
for _, id in InputService:GetGamepads() do
    while #seats < math.min(id, 4) do
        if join() == nil then
            break
        end
    end
end

local menu = Instance.new("InputContext")
menu.Name = "Menu"
menu.Parent = InputService
local function button(name: string, codes: { EnumKeyCode }, pressed: () -> ())
    local action = Instance.new("InputAction")
    action.Name = name
    action.Type = Enum.InputActionType.Bool
    action.Parent = menu
    for _, code in codes do
        local binding = Instance.new("InputBinding")
        binding.KeyCode = code
        binding.Parent = action
    end
    action.Pressed:Connect(pressed)
end
button("NextMode", { Enum.KeyCode.Tab, Enum.KeyCode.ButtonNorth }, function()
    mode = mode % #Modes + 1
    share()
end)
button("Join", { Enum.KeyCode.Return }, function()
    if #seats < 2 then
        join()
    end
end)

RunService.Heartbeat:Connect(function(dt: number)
    local centre = vector.zero
    for _, each in seats do
        local move = each.Player:GetIntent("Move")
        if typeof(move) == "Vector2" and demo.apart == nil then
            local at = each.Body.Position
            -- A stick's up is away from the camera, which is -Z here.
            each.Body.Position = vector.create(
                math.clamp(at.x + move.X * Speed * dt, -Half + 1, Half - 1),
                at.y,
                math.clamp(at.z - move.Y * Speed * dt, -Half + 1, Half - 1)
            )
        end
        each.Own.CFrame = CFrame.lookAt(each.Body.Position + Offset, each.Body.Position)
        centre += each.Body.Position
    end
    local held = screen
    if held ~= nil then
        held:Update(dt)
    elseif #seats > 0 then
        -- One picture, or a fixed layout: the overview keeps everybody's middle.
        centre /= #seats
        overview.CFrame = CFrame.lookAt(centre + Offset * 1.6, centre)
    end
end)
