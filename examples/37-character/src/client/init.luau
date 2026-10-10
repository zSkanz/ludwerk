--!strict
--[[
	This machine's half: the keys, a camera behind whoever this machine plays,
	and a count of footsteps. A `Script` under `GlobalScriptService.Client`,
	from `src/client/`.

	**It reads input and moves nothing.** What the keys say reaches the server
	as intent (`Player:GetIntent` in `src/server/init.luau`), the server walks
	the character, and the character's animation graph mixes its clips from
	how the body then moves -- on this machine and on every other, each from
	what already arrives. No line here or there plays a clip.

	The one thing this script hears of the animation is `EventReached`: the
	graph names the two moments of a walk and of a run a foot lands at
	(`Step`), and a game would play a footstep there. This one counts them.
]]

local Lighting = game:GetService("Lighting")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 14.5
Lighting.GeographicLatitude = 35
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- --- The keys ------------------------------------------------------------------------

local Gameplay = Instance.new("InputContext")
Gameplay.Name = "Gameplay"
Gameplay.Parent = workspace

local function action(name: string, type: EnumInputActionType): InputAction
    local made = Instance.new("InputAction")
    made.Name = name
    made.Type = type
    made.Parent = Gameplay
    return made
end

local function key(on: InputAction, code: EnumKeyCode)
    local binding = Instance.new("InputBinding")
    binding.KeyCode = code
    binding.Parent = on
end

local function keys(on: InputAction, left: EnumKeyCode, right: EnumKeyCode, up: EnumKeyCode?, down: EnumKeyCode?)
    local binding = Instance.new("InputBinding")
    binding.Left = left
    binding.Right = right
    if up and down then
        binding.Up = up
        binding.Down = down
    end
    binding.Parent = on
end

-- Forwards, back and sideways, as the hero faces: W runs, S backs away, A and
-- D step to the side.
local Move = action("Move", Enum.InputActionType.Direction2D)
keys(Move, Enum.KeyCode.A, Enum.KeyCode.D, Enum.KeyCode.W, Enum.KeyCode.S)
key(Move, Enum.KeyCode.LeftThumbstick)

-- Which way the hero faces. Its own action, so that walking sideways is a
-- thing the keys can ask for: only X is read.
local Turn = action("Turn", Enum.InputActionType.Direction2D)
keys(Turn, Enum.KeyCode.Q, Enum.KeyCode.E)
keys(Turn, Enum.KeyCode.Left, Enum.KeyCode.Right)
key(Turn, Enum.KeyCode.RightThumbstick)

local Buttons: { [string]: { EnumKeyCode } } = {
    Jump = { Enum.KeyCode.Space, Enum.KeyCode.ButtonSouth },
    Shoot = { Enum.KeyCode.F, Enum.KeyCode.MouseLeft, Enum.KeyCode.ButtonWest },
    Walk = { Enum.KeyCode.LeftShift, Enum.KeyCode.ButtonEast },
    -- The crossbow to the other hand, and one head for another: what the
    -- hero holds and wears is the server's to change.
    Hand = { Enum.KeyCode.G, Enum.KeyCode.ButtonNorth },
    Hood = { Enum.KeyCode.H, Enum.KeyCode.ButtonBack },
}
for name, codes in Buttons do
    local made = action(name, Enum.InputActionType.Bool)
    for _, code in codes do
        key(made, code)
    end
end

-- --- The count of footsteps -----------------------------------------------------------

local screen = Instance.new("ScreenGui")
screen.Name = "Hud"
screen.Parent = UIService

local function line(text: string, y: number): TextLabel
    local label = Instance.new("TextLabel")
    label.Size = UDim2.new(0, 960, 0, 28)
    label.Position = UDim2.new(0, 16, 0, y)
    label.Text = text
    label.TextSize = 18
    label.TextColor = Color3.fromRGB(240, 240, 232)
    label.TextXAlignment = Enum.HorizontalAlignment.Left
    label.BackgroundTransparency = 1
    label.Parent = screen
    return label
end

line("WASD move   Q / E turn   Shift walk   Space jump   F or click shoot   G other hand   H hood", 12)
local counter = line("steps: 0", 40)
local steps = 0

-- Whose steps: the character the server gave this machine's player, once it
-- has. `EventReached` is this machine's own -- every machine fires its own
-- for the graph it steps -- which is right for a sound and a number on a
-- screen, and is why nothing the rules depend on is decided here.
local heard: BasePart? = nil

local function hear(character: BasePart)
    heard = character
    local animation = character:FindFirstChildOfClass("AnimationPlayer") :: AnimationPlayer?
    if animation == nil then
        return
    end
    animation.EventReached:Connect(function(name: string, _layer: string)
        if name == "Step" then
            steps += 1
            counter.Text = `steps: {steps}`
        end
    end)
end

-- --- The camera --------------------------------------------------------------------------

local camera = workspace:WaitForChild("MainCamera") :: Camera
local Behind = 5
local Above = 2.2

-- Behind and above whoever this machine plays, turning as they turn. Where
-- the character is DRAWN this frame, not where the last tick left it: a
-- camera that followed the tick would shake against the body it follows.
RunService.PreRender:Connect(function()
    local me = NetworkService.LocalPlayer
    local character = me and me.Character
    if character == nil then
        return
    end
    if heard ~= character then
        hear(character)
    end
    local drawn = character:GetRenderCFrame()
    local at = drawn.Position
    camera.CFrame =
        CFrame.lookAt(at - drawn.LookVector * Behind + vector.create(0, Above, 0), at + vector.create(0, 0.5, 0))
end)
