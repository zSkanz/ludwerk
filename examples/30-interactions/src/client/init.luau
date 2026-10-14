--!strict
--[[
	This machine's half (ADR 0105): the keys that walk and use, a camera over
	the shoulder of this player's character, the hint over the door when it is
	in reach, and the pointer's ray while this player holds the drawer or the
	lever. The server's script decides what any of it does.
]]

local InputService = game:GetService("InputService")
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

-- Held by the door to open or shut it: E, or the gamepad's west button.
local UseAction = Instance.new("InputAction")
UseAction.Name = "Use"
UseAction.Type = Enum.InputActionType.Bool
UseAction.Parent = Gameplay
for _, key in { Enum.KeyCode.E, Enum.KeyCode.ButtonWest } do
    local binding = Instance.new("InputBinding")
    binding.KeyCode = key
    binding.Parent = UseAction
end

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

-- --- The hint over the door -------------------------------------------------------

local door = workspace:WaitForChild("Door") :: BasePart
local hint = Instance.new("BillboardGui")
hint.Size = UDim2.new(0, 160, 0, 36)
hint.WorldOffset = vector.create(0, 2.4, 0)
hint.Adornee = door
hint.Enabled = false
hint.Parent = door
local hintText = Instance.new("TextLabel")
hintText.Size = UDim2.new(1, 0, 1, 0)
hintText.TextSize = 16
hintText.Parent = hint

RunService.Heartbeat:Connect(function()
    local me = NetworkService.LocalPlayer
    local character = me and me.Character
    local reach = (door:GetAttribute("Reach") :: number?) or 4
    local near = character ~= nil
        and character:IsA("BasePart")
        and vector.magnitude((character :: BasePart).Position - door.Position) <= reach
    hint.Enabled = near
    if near then
        hintText.Text = if door:GetAttribute("Open") == true then "Hold E: close" else "Hold E: open"
    end
end)

-- --- Holding the drawer or the lever --------------------------------------------------

-- A click takes hold and the next lets go, on this machine as on the server:
-- the same `ClickDetector` fires here for the click this machine made.
local pointer = workspace:WaitForChild("Pointer") :: RemoteEvent
local holding: BasePart? = nil
for _, name in { "Drawer", "Lever" } do
    local part = workspace:WaitForChild(name) :: BasePart
    local handle = part:WaitForChild("ClickDetector") :: ClickDetector
    handle.MouseClick:Connect(function(player: Player)
        if player ~= NetworkService.LocalPlayer then
            return
        end
        holding = if holding == part then nil else part
    end)
end

RunService.Heartbeat:Connect(function()
    if holding == nil then
        return
    end
    local origin, direction = camera:ViewportPointToRay(InputService:GetPointerPosition())
    pointer:FireServer(origin, direction)
end)
