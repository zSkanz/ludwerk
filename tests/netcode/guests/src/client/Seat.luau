--!strict
--[[
	The joiner, with somebody else at its screen: it seats a guest in the
	match it joined, drives the two by code -- its own player to the right,
	the guest forwards, and a button that is the guest's alone -- says a word
	as each, and gives the guest's seat back.

	Lines `[guests] ...`, which `run_guests.cmake` reads.
]]

local NetworkService = game:GetService("NetworkService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

if NetworkService.Authority then
    return
end

local talk = ReplicatedStorage:WaitForChild("Talk") :: RemoteEvent
local board = Workspace:WaitForChild("Board") :: Part
local first = NetworkService.LocalPlayer :: Player

-- What the server wrote down, said here: the driver hears this end.
local function server(name: string)
    print(`[guests] server {name} {board:GetAttribute(name) or "nothing"}`)
end

-- **A seat asked for in a match**: a player at once, and the authority's
-- number for it a moment later.
local guest = NetworkService:AddLocalPlayer() :: Player
assert(guest ~= nil and guest ~= first, "a joiner seats nobody else")
local waited = 0
while guest.UserId >= 0x80000000 and waited < 300 do
    task.wait()
    waited += 1
end
print(
    `[guests] seated first={first.UserId} guest={guest.UserId} locals={#NetworkService:GetLocalPlayers()} `
        .. `players={#NetworkService:GetPlayers()}`
)

talk.ClientReceived:Connect(function(text: string)
    print(`[guests] client heard as=first text={text}`)
end)
talk.LocalClientReceived:Connect(function(player: Player, text: string)
    print(`[guests] client heard as={if player == guest then "guest" else "other"} text={text}`)
end)

local pressing = false
RunService:BindToIntent("First", function(intent: IntentWriter)
    intent:Set("Move", Vector2.new(1, 0))
end)
RunService:BindToIntent("Guest", function(intent: IntentWriter)
    intent:Set("Move", Vector2.new(0, 1))
    intent:Set("Act", pressing)
end, guest)

talk:FireServer("mine")
talk:FireServerFor(guest, "theirs")

task.wait(1)
pressing = true
task.wait(0.25)
pressing = false
task.wait(1)

-- What this machine sees of the two: each player's `Character` is the marker
-- the authority gave it, and the guest's is where the authority has it.
local function place(player: Player): string
    local body = player.Character
    if body == nil then
        return "none"
    end
    local at = body.Position
    return `{string.format("%.1f", at.x)},{string.format("%.1f", at.z)}`
end
print(`[guests] client sees first={place(first)} guest={place(guest)}`)
for _, name in { "Joined1", "Joined2", "Heard1", "Heard2", "Seat1", "Seat2" } do
    server(name)
end

RunService:UnbindFromIntent("Guest")
print(`[guests] removed={NetworkService:RemoveLocalPlayer(guest)}`)
task.wait(0.5)
print(`[guests] after locals={#NetworkService:GetLocalPlayers()} players={#NetworkService:GetPlayers()}`)
server("Left2")
server("Left1")
print("[guests] done")
-- Nothing more to see: the run ends here and the server's with it.
game:Shutdown()
