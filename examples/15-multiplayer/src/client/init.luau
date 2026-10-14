--!strict
--[[
	Two copies of one game, one of them deciding.

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

	**G seats a second player at this machine** (ADR 0205): the arrow keys, or
	a second controller's stick. In somebody else's match the guest is a
	player of that match, with a racer of its own -- and this machine predicts
	both its racers, each from its own player's keys (ADR 0208).
]]

local Lighting = game:GetService("Lighting")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")
local Workspace = game:GetService("Workspace")

local Racing = require("@shared/Racing")

-- What a test asks of it (`--scene-data={"guest":true}`): seat the guest with
-- no key pressed, drive both racers for two seconds, and say how far each went.
type Demo = { guest: boolean? }
local demo = (SceneService:GetLoadData() or {}) :: Demo

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
local seatAction = Instance.new("InputAction")
seatAction.Name = "Seat"
seatAction.Type = Enum.InputActionType.Bool
local keyG = Instance.new("InputBinding")
keyG.KeyCode = Enum.KeyCode.G
keyG.Parent = seatAction
seatAction.Parent = controls
controls.Parent = Workspace

-- **A second player at this machine.** A `Player` of its own, asked for; and
-- controls of its own, a context that is that player's: what it resolves is
-- the guest's intent, and travels to the authority as the guest's.
local guest: Player? = nil
local function seat()
    if guest ~= nil then
        return
    end
    local seated = NetworkService:AddLocalPlayer()
    if seated == nil then
        return
    end
    guest = seated
    seated.GamepadId = 2
    local theirs = Instance.new("InputContext")
    theirs.Name = "GuestControls"
    theirs.Player = seated
    theirs.GamepadId = 2
    local steer = Instance.new("InputAction")
    steer.Name = "Move"
    steer.Type = Enum.InputActionType.Direction2D
    local arrows = Instance.new("InputBinding")
    arrows.Up = Enum.KeyCode.Up
    arrows.Down = Enum.KeyCode.Down
    arrows.Left = Enum.KeyCode.Left
    arrows.Right = Enum.KeyCode.Right
    arrows.Parent = steer
    local pad = Instance.new("InputBinding")
    pad.KeyCode = Enum.KeyCode.LeftThumbstick
    pad.Parent = steer
    steer.Parent = theirs
    theirs.Parent = Workspace
    print("multiplayer: a second player sat down at this machine")
end
seatAction.Pressed:Connect(seat)

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

-- **Every player at this machine, its own racer** (ADR 0208): the guest's is
-- predicted as the first player's is, so both answer their keys at once
-- whatever the link, and the authority corrects each by itself.
if not NetworkService.Authority then
    RunService.Heartbeat:Connect(function(dt: number)
        for _, player in NetworkService:GetLocalPlayers() do
            local racer = player.Character
            if racer then
                Racing.drive(player, racer, dt)
            end
        end
    end)
end

-- The test's run: both sticks held to the right for two seconds, by code, and
-- then how far each racer has gone on THIS machine -- sixteen metres at eight
-- a second, for a racer that is predicted. One that is not is where the
-- authority last said, a round trip short of that.
if demo.guest then
    task.spawn(function()
        while not NetworkService.Authority and NetworkService.State ~= Enum.NetworkState.Connected do
            task.wait()
        end
        seat()
        local first = NetworkService.LocalPlayer :: Player
        local second = guest :: Player
        while first.Character == nil or second.Character == nil do
            task.wait()
        end
        task.wait(1)
        local driving = false
        local function hold(intent: IntentWriter)
            intent:Set("Move", Vector2.new(if driving then 1 else 0, 0))
        end
        RunService:BindToIntent("DemoFirst", hold)
        RunService:BindToIntent("DemoGuest", hold, second)
        local firstRacer = first.Character :: BasePart
        local guestRacer = second.Character :: BasePart
        local firstFrom, guestFrom = firstRacer.Position.x, guestRacer.Position.x
        local ticks = 0
        local done = false
        local watch: Connection
        watch = RunService.Heartbeat:Connect(function()
            driving = ticks < 120
            ticks += 1
            -- Two ticks after the sticks are let go: the last intent written
            -- is the next tick's, and that tick's step is read after it.
            if ticks == 123 then
                watch:Disconnect()
                done = true
            end
        end)
        while not done do
            task.wait()
        end
        local predicted = #NetworkService:GetStats().LocalPlayers
        print(
            string.format(
                "multiplayer: demo, %d racer(s) predicted here, the first went %.1f m and the guest %.1f m",
                predicted,
                firstRacer.Position.x - firstFrom,
                guestRacer.Position.x - guestFrom
            )
        )
    end)
end
