--!strict
--[[
	A room for a headset (ADRs 0244 to 0246). Everything a VR game does with
	its hands, in one script:

	- **Picking up and throwing**: a controller's grip held near a crate or a
	  ball picks it up; it follows the hand, and let go it flies on at the
	  speed the hand was moving -- the last few ticks' travel.
	- **Moving**: `@engine/xrcomfort` -- the left stick forward throws an arc
	  and teleports there, the right stick turns in steps.
	- **An interface**: a `ScreenGui` is a panel in front of the player in a
	  headset, pointed at with the right controller's beam and clicked with
	  its trigger, or pressed with a fingertip.
	- **The room seen through**: on a headset with cameras, a button shows
	  the player's real room behind this one.

	With no headset the room is drawn on the screen, and says how to start
	it in one. `--scene-data={"autopilot":true}` with `--xr=simulated` picks
	up the ball and throws it on its own, and says how far it went -- what
	`example_xr_room_throws` asks of it.
]]

local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")
local XRService = game:GetService("XRService")
local xrcomfort = require("@engine/xrcomfort")

type Demo = { autopilot: boolean? }
local Demo = (SceneService:GetLoadData() or {}) :: Demo

-- How near a hand must be to what it picks up, in metres.
local GrabReach = 0.2
-- How many ticks of a hand's travel its throw is measured over.
local ThrowTicks = 4

local function block(name: string, size: vector, position: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Anchored = true
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    return part
end

-- The room: a floor, four low walls and a table.
block("Floor", vector.create(8, 0.2, 8), vector.create(0, -0.1, 0), Color3.fromRGB(150, 140, 125))
block("WallNorth", vector.create(8, 1.2, 0.2), vector.create(0, 0.6, -4), Color3.fromRGB(205, 200, 190))
block("WallSouth", vector.create(8, 1.2, 0.2), vector.create(0, 0.6, 4), Color3.fromRGB(205, 200, 190))
block("WallEast", vector.create(0.2, 1.2, 8), vector.create(4, 0.6, 0), Color3.fromRGB(205, 200, 190))
block("WallWest", vector.create(0.2, 1.2, 8), vector.create(-4, 0.6, 0), Color3.fromRGB(205, 200, 190))
block("Table", vector.create(1.4, 0.8, 0.7), vector.create(0, 0.4, -1.2), Color3.fromRGB(120, 84, 60))

-- What there is to pick up, and where each goes back to.
type Throwable = { part: Part, home: CFrame }
local Throwables: { Throwable } = {}
local function throwable(name: string, shape: EnumPartShape, size: number, position: vector, color: Color3)
    local part = Instance.new("Part")
    part.Name = name
    part.Shape = shape
    part.Size = vector.create(size, size, size)
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
    table.insert(Throwables, { part = part, home = part.CFrame })
end
throwable("Ball", Enum.PartShape.Ball, 0.12, vector.create(0.3, 0.86, -1.2), Color3.fromRGB(230, 70, 60))
throwable("CrateA", Enum.PartShape.Block, 0.15, vector.create(-0.3, 0.88, -1.2), Color3.fromRGB(70, 130, 220))
throwable("CrateB", Enum.PartShape.Block, 0.15, vector.create(-0.05, 0.88, -1.1), Color3.fromRGB(240, 200, 60))
throwable("BallSmall", Enum.PartShape.Ball, 0.08, vector.create(0.5, 0.84, -1.05), Color3.fromRGB(90, 200, 120))

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera

-- **Moving**, in a headset; flat, a view of the room from its door.
local session: xrcomfort.Session? = nil
local smoothTurn = false
local function startComfort()
    if session then
        session:Stop()
    end
    session = xrcomfort.start({ Move = "teleport", Turn = if smoothTurn then "smooth" else "snap", Camera = camera })
end
camera.CFrame = CFrame.lookAt(vector.create(0, 1.7, 3), vector.create(0, 0.8, -1.2))

-- **The panel**: in a headset, the screen is a panel in front of the player.
local gui = Instance.new("ScreenGui")
gui.Name = "RoomPanel"
gui.XRDistance = 1.2
gui.Parent = UIService

local frame = Instance.new("Frame")
frame.Size = UDim2.fromScale(0.5, 0.5)
frame.Position = UDim2.fromScale(0.25, 0.25)
frame.BackgroundColor = Color3.fromRGB(28, 32, 40)
frame.BackgroundTransparency = 0.1
frame.Parent = gui

local function text(name: string, kind: string, y: number, height: number, size: number): TextLabel
    -- A button has every property a label has, and is clicked.
    local made: TextLabel = if kind == "TextButton"
        then Instance.new("TextButton") :: any
        else Instance.new("TextLabel")
    made.Name = name
    made.Position = UDim2.new(0.05, 0, y, 0)
    made.Size = UDim2.new(0.9, 0, 0, height)
    made.TextSize = size
    made.TextColor = Color3.new(1, 1, 1)
    made.TextWrapped = true
    made.BackgroundTransparency = if kind == "TextButton" then 0 else 1
    made.BackgroundColor = Color3.fromRGB(60, 90, 140)
    made.Parent = frame
    return made
end
local title = text("Title", "TextLabel", 0.04, 48, 36)
local help = text("Help", "TextLabel", 0.18, 80, 20)
local reset = text("Reset", "TextButton", 0.45, 52, 24) :: any
local turning = text("Turning", "TextButton", 0.62, 52, 24) :: any
local passthrough = text("Passthrough", "TextButton", 0.79, 52, 24) :: any

local function say()
    title.Text = LocalizationService:Translate("panel.title")
    help.Text = if XRService.Enabled
        then LocalizationService:Translate("panel.help")
        else LocalizationService:Translate("flat.hint")
    reset.Text = LocalizationService:Translate("panel.reset")
    turning.Text = LocalizationService:Translate(if smoothTurn then "panel.turn.smooth" else "panel.turn.snap")
    passthrough.Text = if not XRService:SupportsPassthrough()
        then LocalizationService:Translate("panel.passthrough.none")
        elseif XRService.Passthrough then LocalizationService:Translate("panel.passthrough.on")
        else LocalizationService:Translate("panel.passthrough.off")
end
say()
LocalizationService.LocaleChanged:Connect(say)

-- **Into the headset** when its session starts -- the engine knows whether
-- there is one only from its first frame: the play space on the room's floor,
-- and moving from then on.
local function enter()
    if XRService.Enabled and session == nil then
        camera.CFrame = CFrame.new(0, 0, 0)
        startComfort()
        say()
    end
end
XRService.SessionStateChanged:Connect(enter)
enter()

reset.Activated:Connect(function()
    for _, one in Throwables do
        one.part.Anchored = false
        one.part.CFrame = one.home
        one.part.LinearVelocity = vector.zero
    end
end)
-- The room seen through, where the headset has cameras for it: the walls of
-- this one stand in the player's real room.
passthrough.Activated:Connect(function()
    if XRService:SupportsPassthrough() then
        XRService.Passthrough = not XRService.Passthrough
        say()
    end
end)
turning.Activated:Connect(function()
    smoothTurn = not smoothTurn
    say()
    if XRService.Enabled then
        startComfort()
    end
end)

-- **The hands**: a small sphere where each is, so the player sees them.
local function marker(name: string): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Shape = Enum.PartShape.Ball
    part.Size = vector.create(0.06, 0.06, 0.06)
    part.Anchored = true
    part.CanCollide = false
    part.CanQuery = false
    part.CanTouch = false
    part:SetMaterialParameter("Color", Color3.fromRGB(235, 235, 240))
    part.Parent = workspace
    return part
end
-- Each hand by its place: 1 the left, 2 the right. An enum item is not a
-- table key that finds itself again.
local Hands = { Enum.XRHand.Left, Enum.XRHand.Right }
local Markers = { marker("LeftHand"), marker("RightHand") }

-- **Picking up**: each hand's grip, as an action.
local context = Instance.new("InputContext")
context.Name = "Hands"
context.Parent = workspace
local function grip(name: string, key: EnumKeyCode): InputAction
    local action = Instance.new("InputAction")
    action.Name = name
    action.Type = Enum.InputActionType.Bool
    action.Parent = context
    local binding = Instance.new("InputBinding")
    binding.KeyCode = key
    binding.Parent = action
    return action
end
local Grips = { grip("GripLeft", Enum.KeyCode.XRGripLeft), grip("GripRight", Enum.KeyCode.XRGripRight) }

type Hold = { thing: Throwable, offset: CFrame, trail: { vector } }
local Holds: { [number]: Hold? } = {}

local function handInWorld(hand: EnumXRHand): CFrame?
    if not XRService:IsHandTracked(hand) then
        return nil
    end
    return camera.CFrame * XRService:GetHandCFrame(hand)
end

local function nearest(at: vector): Throwable?
    local best: Throwable? = nil
    local bestDistance = GrabReach
    for _, one in Throwables do
        local held = false
        for _, hold in Holds do
            held = held or (hold ~= nil and hold.thing == one)
        end
        local distance = vector.magnitude(one.part.Position - at)
        if not held and distance < bestDistance then
            best = one
            bestDistance = distance
        end
    end
    return best
end

-- Once a tick: what each hand picks up, carries and lets go of.
RunService.PreSimulation:Connect(function(dt: number)
    for index, action in Grips do
        local at = handInWorld(Hands[index])
        local held = Holds[index]
        local down = action:GetState() == true
        if at and down and not held then
            local thing = nearest(at.Position)
            if thing then
                thing.part.Anchored = true
                Holds[index] = { thing = thing, offset = at:Inverse() * thing.part.CFrame, trail = {} }
            end
        elseif held and (not down or not at) then
            -- Let go: on at the speed the hand was moving.
            Holds[index] = nil
            local part = held.thing.part
            part.Anchored = false
            local trail = held.trail
            if #trail >= 2 then
                part.LinearVelocity = (trail[#trail] - trail[1]) / ((#trail - 1) * dt)
            end
        elseif held and at then
            local part = held.thing.part
            part.CFrame = at * held.offset
            table.insert(held.trail, part.Position)
            if #held.trail > ThrowTicks then
                table.remove(held.trail, 1)
            end
        end
    end
end)

-- Every frame: the comfort session where no frame draws it, and the hands.
RunService.Heartbeat:Connect(function(dt: number)
    if session then
        session:Update(dt)
    end
    for index, ball in Markers do
        local at = handInWorld(Hands[index])
        ball.Fade = if at then 0 else 1
        if at then
            ball.CFrame = at
        end
    end
end)

-- **The autopilot**: the right hand to the ball, the grip closed, a quick
-- swing forward and up, and the grip opened -- then how far the ball went.
if Demo.autopilot then
    local ball = Throwables[1].part
    local from = ball.Position
    local ticks = 0
    RunService.PostSimulation:Connect(function()
        ticks += 1
        if ticks == 10 then
            XRService:SimulatePose(Enum.XRPose.RightHand, CFrame.new(from.x, from.y, from.z))
        elseif ticks == 14 then
            XRService:SimulateInput(Enum.KeyCode.XRGripRight, 1)
        elseif ticks > 18 and ticks <= 26 then
            local step = ticks - 18
            XRService:SimulatePose(Enum.XRPose.RightHand, CFrame.new(from.x, from.y + 0.04 * step, from.z - 0.1 * step))
        elseif ticks == 27 then
            XRService:SimulateInput(Enum.KeyCode.XRGripRight, 0)
        elseif ticks == 150 then
            local moved = ball.Position - from
            print(`threw the ball {string.format("%.1f", vector.magnitude(vector.create(moved.x, 0, moved.z)))} m`)
        end
    end)
end
