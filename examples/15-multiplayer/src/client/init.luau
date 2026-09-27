--!strict
--[[
	Two copies of one game, one of them deciding (N1).

	The same project runs in every posture -- the command line picks which:

	    run.bat --host                    a window, and the authority
	    run.bat --join=127.0.0.1          a window showing somebody else's world
	    run.bat --serve                   the authority with no window at all

	**Where a script is decides where it runs** (ADR 0105). The rules are in
	`src/server/`, which runs only where the world is decided -- solo, a host,
	a dedicated server. This file is in `src/client/`, which runs where a player
	sits: it makes the controls and points a camera, and a client that joined
	builds nothing, because everything that replicates arrives from the
	authority. What its player presses travels there as intent, and the
	authority moves that player's racer. WASD or the left stick.

	**Space honks**, and that is a message rather than intent (ADR 0077): a
	`RemoteEvent` the authority made carries "I honked" to it, the authority
	makes that player's racer hop -- which every window sees, because it is
	the world -- and tells every machine who it was.

	**H asks a question and waits for the answer** (ADR 0079): a
	`RemoteFunction` returns how many times this player has honked, which only
	the authority counts.
]]

local Lighting = game:GetService("Lighting")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Racing = require("@shared/Racing")

-- **Every copy makes the controls**, the replica included: what a player
-- presses is resolved on their own machine, and the engine sends the result to
-- the authority as intent. A Simulation-rate context, because intent is what
-- the simulation clock resolved -- a Render-rate one is a camera's business.
local controls = Instance.new("InputContext")
controls.Name = "Controls"
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
local honkAction = Instance.new("InputAction")
honkAction.Name = "Honk"
honkAction.Type = Enum.InputActionType.Bool
local space = Instance.new("InputBinding")
space.KeyCode = Enum.KeyCode.Space
space.Parent = honkAction
honkAction.Parent = controls
local standingAction = Instance.new("InputAction")
standingAction.Name = "Standing"
standingAction.Type = Enum.InputActionType.Bool
local keyH = Instance.new("InputBinding")
keyH.KeyCode = Enum.KeyCode.H
keyH.Parent = standingAction
standingAction.Parent = controls
controls.Parent = Workspace

local camera = Instance.new("Camera")
camera.Name = "Camera"
camera.CFrame = CFrame.lookAt(vector.create(0, 14, 22), vector.create(0, 2, 0))
camera.Parent = Workspace
Workspace.CurrentCamera = camera
Lighting.ClockTime = 14

print(`multiplayer: running as {NetworkService.Topology.Name}, authority = {NetworkService.Authority}`)

-- **The replica predicts its own racer** (ADR 0076). The authority says which
-- part is whose through `Player.Character`; the replica moves its own at once
-- from its own keys, exactly as the authority will move it when the intent
-- arrives, and the authority's snapshots correct it rather than overwrite it.
-- Everything else a replica shows is what it is sent, drawn between snapshots.

-- The honk: the key sends a message to whoever decides, and every machine
-- hears who honked. The server's script makes the event; this waits for it,
-- which on a host is at once and on a client is when it arrives.
local honk = Workspace:WaitForChild("Honk") :: RemoteEvent
honkAction.Pressed:Connect(function()
    honk:FireServer()
end)
honk.ClientReceived:Connect(function(who: string)
    print(`multiplayer: {who} honked`)
end)

-- A question, where a honk is a statement: the handler below answers it on
-- the authority, and this machine's script waits for that answer.
local standing = Workspace:WaitForChild("Standing") :: RemoteFunction
standingAction.Pressed:Connect(function()
    local count = standing:InvokeServerAsync()
    print(`multiplayer: you have honked {count} time(s)`)
end)

if not NetworkService.Authority then
    RunService.Heartbeat:Connect(function(dt: number)
        local me = NetworkService.LocalPlayer
        local racer = me and me.Character
        if me and racer then
            Racing.drive(me, racer, dt)
        end
    end)
end
