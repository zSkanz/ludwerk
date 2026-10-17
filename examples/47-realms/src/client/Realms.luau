--!strict
--[[
	The player's side of every world, under `GlobalScriptService.Client`: it
	lives across worlds, since the hop between them does not stop it.

	- `@engine/travelscreen` covers the screen while the machine goes from one
	  world to the next, and `@engine/announcements` shows what the servers'
	  operator says (`say` in their console, a `stop` counting down).
	- The controls, a camera behind this player's character, and their own
	  character moved at once from their keys.
	- The world's name across the top, and what the world tells this player --
	  an attribute it sets on them -- under it for a few seconds.
]]

local InputService = game:GetService("InputService")
local LocalizationService = game:GetService("LocalizationService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

local announcements = require("@engine/announcements")
local travelscreen = require("@engine/travelscreen")

local Walk = require("@shared/Walk")

travelscreen.mount()
announcements.mount()

local controls = Instance.new("InputContext")
controls.Name = "RealmControls"
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
-- Under `InputService`, which every world shares, rather than a world that
-- the next hop replaces.
controls.Parent = InputService

-- **A camera in each world**: the one before went with the world it was in,
-- so a world that has just loaded is given a new one.
local camera: Camera
local function makeCamera()
    camera = Instance.new("Camera")
    camera.CFrame = CFrame.lookAt(vector.create(0, 16, 24), vector.create(0, 0, 0))
    camera.Parent = workspace
    workspace.CurrentCamera = camera
end
makeCamera()
SceneService.SceneLoaded:Connect(makeCamera)

local CameraOffset = vector.create(0, 11, 15)
RunService.PreRender:Connect(function()
    local me = NetworkService.LocalPlayer
    local character = me and me.Character
    if character and character:IsA("BasePart") and workspace.CurrentCamera == camera then
        local at = character.Position
        camera.CFrame = CFrame.lookAt(at + CameraOffset, at)
    end
end)

if not NetworkService.Authority then
    RunService.Heartbeat:Connect(function(dt: number)
        local me = NetworkService.LocalPlayer
        local character = me and me.Character
        if me and character and character:IsA("BasePart") then
            Walk.step(me, character, dt)
        end
    end)
end

local screen = Instance.new("ScreenGui")
screen.Name = "RealmHud"
screen.KeepOnSceneLoad = true
screen.Parent = UIService

local title = Instance.new("TextLabel")
title.Size = UDim2.new(1, 0, 0, 44)
title.TextSize = 24
title.TextColor = Color3.fromRGB(240, 236, 226)
title.BackgroundTransparency = 1
title.Parent = screen

local notice = Instance.new("TextLabel")
notice.Position = UDim2.new(0, 0, 0, 44)
notice.Size = UDim2.new(1, 0, 0, 28)
notice.TextSize = 18
notice.TextColor = Color3.fromRGB(250, 214, 120)
notice.BackgroundTransparency = 1
notice.Parent = screen

local hint = Instance.new("TextLabel")
hint.Position = UDim2.new(0, 0, 1, -32)
hint.Size = UDim2.new(1, 0, 0, 28)
hint.TextSize = 16
hint.TextColor = Color3.fromRGB(200, 204, 212)
hint.BackgroundTransparency = 1
hint.Text = LocalizationService:Translate("realm.hint")
hint.Parent = screen

-- How long a notice stays, in seconds.
local NoticeSeconds = 4

RunService.PreRender:Connect(function()
    local realm = workspace:GetAttribute("Realm")
    title.Text = if typeof(realm) == "string" then LocalizationService:Translate(`realm.name.{realm}`) else ""
    local me = NetworkService.LocalPlayer
    local key = me and me:GetAttribute("Notice")
    local at = me and me:GetAttribute("NoticeAt")
    if typeof(key) == "string" and typeof(at) == "number" and RunService.SimTime - at < NoticeSeconds then
        notice.Text = LocalizationService:Translate(key)
    else
        notice.Text = ""
    end
end)
