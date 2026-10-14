--!strict
--[[
	The authority: it gives every player a marker that the player's own `Move`
	intent pushes across the floor, counts each one's `Act`, and answers what
	each says through a `RemoteEvent` to the one who said it.

	Two players come from ONE machine: its own and a guest at the same screen
	(protocol 48). What this holds is that they are two players here in every
	way that matters -- two numbers, two intents a tick, two senders, two
	addressees -- and that the second leaves by itself.

	Its account is left on `Board` as attributes, a fact each, where the
	joiner reads it and says it beside its own: the driver hears one end.
]]

local NetworkService = game:GetService("NetworkService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local talk = Instance.new("RemoteEvent")
talk.Name = "Talk"
talk.Parent = ReplicatedStorage

local board = Instance.new("Part")
board.Name = "Board"
board.Anchored = true
board.Position = vector.create(0, -20, 0)
board.Parent = Workspace

type Seat = { Order: number, Marker: Part, Acts: number, Ticks: number }
local seats: { [Player]: Seat } = {}
local joined = 0

local function account(player: Player, seat: Seat): string
    local at = seat.Marker.Position
    return `user={player.UserId} x={string.format("%.2f", at.x)} z={string.format("%.2f", at.z)} `
        .. `acts={seat.Acts} moved={seat.Ticks}`
end

NetworkService.PlayerAdded:Connect(function(player: Player)
    joined += 1
    local marker = Instance.new("Part")
    marker.Name = `Marker{joined}`
    marker.Anchored = true
    marker.Size = vector.create(1, 1, 1)
    marker.Position = vector.create(0, 0.5, 0)
    marker.Parent = Workspace
    player.Character = marker
    seats[player] = { Order = joined, Marker = marker, Acts = 0, Ticks = 0 }
    board:SetAttribute(`Joined{joined}`, `user={player.UserId} players={#NetworkService:GetPlayers()}`)
end)

NetworkService.PlayerRemoving:Connect(function(player: Player)
    local seat = seats[player]
    if seat == nil then
        return
    end
    board:SetAttribute(`Left{seat.Order}`, account(player, seat))
    seats[player] = nil
    -- The machine's own player gone is the machine gone: there is nobody else.
    if seat.Order == 1 then
        game:Shutdown()
    end
end)

talk.ServerReceived:Connect(function(player: Player, text: string)
    local seat = seats[player]
    board:SetAttribute(`Heard{if seat then seat.Order else 0}`, `user={player.UserId} text={text}`)
    talk:FireClient(player, `echo {text}`)
end)

-- Sixteenths, so every place is exact in a float.
local Step = 1 / 16
RunService.Heartbeat:Connect(function()
    for player, seat in seats do
        local move = player:GetIntent("Move")
        if typeof(move) == "Vector2" and (move.X ~= 0 or move.Y ~= 0) then
            local at = seat.Marker.Position
            seat.Marker.Position = vector.create(at.x + move.X * Step, at.y, at.z + move.Y * Step)
            seat.Ticks += 1
        end
        if player:GetIntent("Act") == true then
            seat.Acts += 1
        end
        board:SetAttribute(`Seat{seat.Order}`, account(player, seat))
    end
end)
