--!strict
--[[
	The rules (ADR 0105): what only the authority decides.

	A `Script` under `GlobalScriptService.Server`, from `src/server/`. It runs
	solo, on a host and on a dedicated server -- wherever this machine decides
	the world -- and never on a client that joined: it is not sent there, and a
	dedicated export leaves it out of the client's package. That is what the
	`if NetworkService.Authority` this example used to carry was standing in for.

	It builds the arena, makes one racer per player, answers the honk and the
	standing question, and moves everything every tick. The client's own script
	(`src/client/`) makes the controls and the camera.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Racing = require("@shared/Racing")

--- How many crates stand in the ring, and how far out.
local RingCount = 12
local RingRadius = 9

-- The messages every machine uses: made here, where they are answered, and
-- waited for by the clients' own script.
local honk = Instance.new("RemoteEvent")
honk.Name = "Honk"
honk.Parent = Workspace
local standing = Instance.new("RemoteFunction")
standing.Name = "Standing"
standing.Parent = Workspace

local ground = Instance.new("Part")
ground.Name = "Ground"
ground.Anchored = true
ground.Size = vector.create(40, 1, 40)
ground.Position = vector.create(0, -0.5, 0)
ground:SetMaterialParameter("Color", Color3.fromRGB(96, 110, 96))
ground.Parent = Workspace

local ring = Instance.new("Folder")
ring.Name = "Ring"
ring.Parent = Workspace
for index = 1, RingCount do
    local angle = (index / RingCount) * math.pi * 2
    local crate = Instance.new("Part")
    crate.Name = `Crate{index}`
    crate.Size = vector.create(1.5, 1.5, 1.5)
    crate.Position = vector.create(math.cos(angle) * RingRadius, 4 + index * 0.5, math.sin(angle) * RingRadius)
    crate:SetMaterialParameter("Color", Color3.fromHSV(index / RingCount, 0.7, 0.9))
    crate.Parent = ring
end

-- **Moved by the authority, every tick.** Anchored, so the solver leaves it to
-- this script; the replica sees it turn because its transform is in every
-- snapshot that changes it, and nothing else.
local spinner = Instance.new("Part")
spinner.Name = "Spinner"
spinner.Anchored = true
spinner.Size = vector.create(12, 0.6, 1)
spinner:SetMaterialParameter("Color", Color3.fromRGB(230, 170, 40))
spinner.Parent = Workspace

-- **One racer per player**, made when they join and gone when they leave.
-- Solo that is one racer, driven by this machine's keys; hosting it is one
-- per window, each driven by what that window's player pressed.
local racers: { [Player]: BasePart } = {}

local function addRacer(player: Player)
    local racer = Instance.new("Part")
    racer.Name = `Racer{player.UserId}`
    racer.Anchored = true
    racer.Size = vector.create(1.2, 1.2, 1.2)
    racer:SetMaterialParameter("Color", Color3.fromHSV((player.UserId * 0.37) % 1, 0.8, 1))
    racer.Position = vector.create((player.UserId - 1) * 3, 0.6, 4)
    racer.Parent = Workspace
    racers[player] = racer
    -- What makes it theirs: their replica predicts it, and is sent what is
    -- near it.
    player.Character = racer
end

for _, player in NetworkService:GetPlayers() do
    addRacer(player)
end

-- A honk makes the honker's racer hop: the SENDER is who the connection says
-- it is, never anything the message claims.
local hops: { [BasePart]: number } = {}
local honks: { [Player]: number } = {}
honk.ServerReceived:Connect(function(player: Player)
    local racer = racers[player]
    if racer then
        hops[racer] = RunService.SimTime
    end
    honks[player] = (honks[player] or 0) + 1
    honk:FireAllClients(player.Name)
end)
standing.OnServerInvoke = function(player: Player): number
    return honks[player] or 0
end
NetworkService.PlayerAdded:Connect(addRacer)
NetworkService.PlayerRemoving:Connect(function(player: Player)
    local racer = racers[player]
    if racer then
        racer:Destroy()
        racers[player] = nil
    end
    honks[player] = nil
end)

RunService.Heartbeat:Connect(function(dt: number)
    local turn = RunService.SimTime * 0.8
    spinner.CFrame = CFrame.new(0, 1.2, 0) * CFrame.fromEuler(0, turn, 0)

    -- What each player DID, read the same way for the one at this machine and
    -- for the ones across the network. The authority decides where that puts
    -- them; the replicas see where it put them.
    for player, racer in racers do
        Racing.drive(player, racer, dt)
        -- Half a second of hop, as an arc.
        local started = hops[racer]
        local height = 0
        if started then
            local t = (RunService.SimTime - started) / 0.5
            if t < 1 then
                height = 4 * t * (1 - t) * 1.5
            else
                hops[racer] = nil
            end
        end
        local at = racer.Position
        racer.Position = vector.create(at.x, 0.6 + height, at.z)
    end
end)
