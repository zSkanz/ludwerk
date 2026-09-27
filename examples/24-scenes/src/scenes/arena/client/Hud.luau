--!strict
--[[
	The arena scene's player code (ADR 0105): its camera and its HUD, from
	`src/scenes/arena/client/`. Escape goes back to the menu, passing the round
	along so the menu offers the next one.
]]

local InputService = game:GetService("InputService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

local Walk = require("@shared/Walk")

local data = SceneService:GetLoadData()
local round = 1
if typeof(data) == "table" and typeof(data.Round) == "number" then
    round = data.Round
end

-- **The controls**, on every machine a player sits at: what they press is
-- resolved here, and reaches the server as intent (`player:GetIntent("Move")`).
local controls = Instance.new("InputContext")
controls.Name = "ArenaControls"
local move = Instance.new("InputAction")
move.Name = "Move"
move.Type = Enum.InputActionType.Direction2D
local keys = Instance.new("InputBinding")
keys.Up = Enum.KeyCode.W
keys.Down = Enum.KeyCode.S
keys.Left = Enum.KeyCode.A
keys.Right = Enum.KeyCode.D
keys.Parent = move
local stick = Instance.new("InputBinding")
stick.KeyCode = Enum.KeyCode.LeftThumbstick
stick.Parent = move
move.Parent = controls
controls.Parent = workspace

local camera = Instance.new("Camera")
camera.CFrame = CFrame.lookAt(vector.create(0, 12, 20), vector.create(0, 1, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- **The camera follows this player's character**, once the server has made it
-- and said whose it is. Until then it looks at the whole arena.
local CameraOffset = vector.create(0, 10, 14)
RunService.PreRender:Connect(function()
    local me = NetworkService.LocalPlayer
    local character = me and me.Character
    if character then
        local at = character.Position
        camera.CFrame = CFrame.lookAt(at + CameraOffset, at)
    end
end)

-- **A client that joined moves its own character at once** (ADR 0076), with
-- the same function the server uses; the server's snapshots correct it rather
-- than overwrite it. Solo and a host ARE the server, whose rules move it.
if not NetworkService.Authority then
    RunService.Heartbeat:Connect(function(dt: number)
        local me = NetworkService.LocalPlayer
        local character = me and me.Character
        if me and character then
            Walk.step(me, character, dt)
        end
    end)
end

local screen = Instance.new("ScreenGui")
screen.Name = "Hud"
screen.Parent = UIService

local status = Instance.new("TextLabel")
status.Size = UDim2.new(1, 0, 0, 40)
status.Text = `Round {round} -- WASD to walk, Escape for the menu`
status.TextSize = 18
status.TextColor = Color3.fromRGB(232, 236, 244)
status.BackgroundTransparency = 1
status.Parent = screen

InputService.InputBegan:Connect(function(input: InputObject, uiConsumed: boolean)
    if uiConsumed or input.KeyCode ~= Enum.KeyCode.Escape then
        return
    end
    -- The scene is the authority's: solo or hosting, go back to the menu; a
    -- client that joined leaves the match instead, and the game's own code
    -- takes it to the menu when `Disconnected` fires.
    if NetworkService.Authority then
        NetworkService:Disconnect()
        SceneService:LoadScene("scenes/menu.scene.json", { Round = round })
    else
        NetworkService:Disconnect()
    end
end)
