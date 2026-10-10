--!strict
--- Two players at two controllers, from a recording (ADR 0195).
---
--- The primary player and one guest, each with an `InputContext` that reads
--- ONE controller -- `GamepadId` 1 and 2 -- and a marker each that its own
--- player's `Move` intent pushes across the floor. The recording drives the
--- two controllers apart: one right, one forward; a button on the second
--- that the first must not hear; and the second unplugged while the first is
--- still held.
---
--- What this holds that a hash cannot: who moved. A replay that fed both
--- contexts the same stick -- or neither, which is what a recording did before
--- it could say whose controller a line was of -- reproduces perfectly and
--- leaves both markers in the same place. So the scene asserts where each
--- ended, and names the fact in an attribute the scenario requires.
---
--- Markers moved by plain adds on anchored parts: nothing here is a solver,
--- so the trace is the same on every compiler and is checked in.

local InputService = game:GetService("InputService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local primary = NetworkService.LocalPlayer :: Player
local guest = NetworkService:AddLocalPlayer() :: Player
assert(guest ~= nil and guest ~= primary, "a guest joins an offline world")

local function marker(name: string, x: number): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Anchored = true
    part.Size = vector.create(1, 1, 1)
    part.CFrame = CFrame.new(x, 0.5, 0)
    part.Parent = workspace
    return part
end

local function seat(name: string, player: Player?, controller: number)
    local context = Instance.new("InputContext")
    context.Name = name
    -- Nil is the primary's, as a game with one player never says whose.
    context.Player = player
    context.GamepadId = controller
    context.Parent = InputService

    local move = Instance.new("InputAction")
    move.Name = "Move"
    move.Type = Enum.InputActionType.Direction2D
    move.Parent = context
    local stick = Instance.new("InputBinding")
    stick.KeyCode = Enum.KeyCode.LeftThumbstick
    stick.Parent = move

    local act = Instance.new("InputAction")
    act.Name = "Act"
    act.Type = Enum.InputActionType.Bool
    act.Parent = context
    local button = Instance.new("InputBinding")
    button.KeyCode = Enum.KeyCode.ButtonSouth
    button.Parent = act
end

seat("PrimarySeat", nil, 1)
seat("GuestSeat", guest, 2)

local markers: { [Player]: Part } = {
    [primary] = marker("PrimaryMarker", -4),
    [guest] = marker("GuestMarker", 4),
}
local acted: { [Player]: number } = { [primary] = 0, [guest] = 0 }
local Speed = 1 / 16

local tick = 0
RunService.Heartbeat:Connect(function()
    tick += 1
    for _, player in { primary, guest } do
        local move = player:GetIntent("Move")
        if typeof(move) == "Vector2" then
            local part = markers[player]
            local at = part.CFrame.Position
            -- Sixteenths, so every place is exact in a float.
            part.CFrame = CFrame.new(at.x + move.X * Speed, at.y, at.z + move.Y * Speed)
        end
        if player:GetIntent("Act") == true then
            acted[player] += 1
        end
    end

    -- **Which controllers there are**, as a game asks to offer a seat: both
    -- once plugged in, the first alone while the second is out, both again.
    if tick == 50 or tick == 300 or tick == 450 then
        local pads = InputService:GetGamepads()
        local expected = if tick == 300 then 1 else 2
        assert(#pads == expected and pads[1] == 1, `{#pads} controllers at tick {tick}`)
        assert(expected == 1 or pads[2] == 2, "the second controller is not the second in the list")
    end

    if tick == 560 then
        local first = markers[primary].CFrame.Position
        local second = markers[guest].CFrame.Position
        -- The first controller was held right for 200 ticks and never along
        -- the other axis: 12.5 metres. The second was pushed along its own Y
        -- for 120 ticks and never sideways -- 7.5 metres, towards -Z, a
        -- stick's +Y being down the screen -- and was then unplugged with the
        -- stick still pushed, which must move its player no further.
        assert(math.abs(first.x - 8.5) < 0.001 and math.abs(first.z) < 0.001, `the primary's marker is at {first}`)
        assert(math.abs(second.x - 4) < 0.001, `the guest's marker went sideways: {second}`)
        assert(math.abs(second.z + 7.5) < 0.001, `the guest's marker is at {second}`)
        -- The button was the second controller's alone.
        assert(acted[primary] == 0, "the primary heard the guest's button")
        assert(acted[guest] > 0, "the guest's button was heard by nobody")
        game:SetAttribute("TwoSeats", true)
    end
end)
