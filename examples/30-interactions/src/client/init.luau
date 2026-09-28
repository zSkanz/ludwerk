--!strict
--[[
	This machine's half (ADR 0105): the keys that walk, and a camera over the
	shoulder of this player's character. Nothing here clicks or prompts -- the
	engine does that from the pointer and the keys, and the server's script
	hears it.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local Gameplay = Instance.new("InputContext")
Gameplay.Name = "Gameplay"
Gameplay.Parent = workspace

local MoveAction = Instance.new("InputAction")
MoveAction.Name = "Move"
MoveAction.Type = Enum.InputActionType.Direction2D
MoveAction.Parent = Gameplay

local MoveKeys = Instance.new("InputBinding")
MoveKeys.Left = Enum.KeyCode.A
MoveKeys.Right = Enum.KeyCode.D
MoveKeys.Up = Enum.KeyCode.W
MoveKeys.Down = Enum.KeyCode.S
MoveKeys.Parent = MoveAction

local MoveStick = Instance.new("InputBinding")
MoveStick.KeyCode = Enum.KeyCode.LeftThumbstick
MoveStick.Parent = MoveAction

local MoveTouch = Instance.new("InputBinding")
MoveTouch.KeyCode = Enum.KeyCode.VirtualStick1
MoveTouch.Parent = MoveAction

local camera = Instance.new("Camera")
camera.FieldOfView = 60
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(0, 7, 12), vector.create(0, 1, 0))

-- Behind and above whoever this machine plays, looking at them.
RunService.PreRender:Connect(function()
    local me = NetworkService.LocalPlayer
    local character = me and me.Character
    if character and character:IsA("BasePart") then
        local at = (character :: BasePart).Position
        camera.CFrame = CFrame.lookAt(at + vector.create(0, 5, 7), at + vector.create(0, 1, 0))
    end
end)
