--!strict
--[[
	The bot: once joined, it walks a straight line for ten seconds, stops,
	and measures -- the corrections its own character took while walking,
	how far the replica stopped from where the authority did, the ping, a
	remote's round trip, and how far in the past the others are drawn. One
	line, `[acceptance] ...`, which the gate's driver reads.

	`Condition` is written by the driver into each run's copy: the name of the
	condition, and a frame to hang in the middle of the walk, in seconds.
]]

local InputService = game:GetService("InputService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Condition = require("./Condition.module")
local Drive = require("@shared/Drive")

local context = Instance.new("InputContext")
context.Parent = Workspace
local move = Instance.new("InputAction")
move.Name = "Move"
move.Type = Enum.InputActionType.Direction2D
move.Parent = context
local stick = Instance.new("InputBinding")
stick.KeyCode = Enum.KeyCode.VirtualStick1
stick.Parent = move

if NetworkService.Authority then
    return
end

-- The client steps its own character with the code the server steps it with.
RunService.Heartbeat:Connect(function()
    local me = NetworkService.LocalPlayer
    local body = me and me.Character
    if me and body and body:IsA("CharacterBody") then
        Drive.step(me, body :: CharacterBody)
    end
end)

-- The corrections taken in `seconds` of simulated time, counted -- and the
-- frames in that time, on either machine, that took longer than three ticks,
-- which drop simulated time and may each cost a correction: the one this run
-- hangs on purpose, and any a busy machine makes of its own.
--
-- `begin`, when there is one, runs once the count has started: what is done in
-- it -- a key going down -- is inside the time measured.
local function corrections(seconds: number, begin: (() -> ())?): (number, number)
    local serverLong = Workspace:WaitForChild("LongFrames") :: RemoteFunction
    local serverBefore = serverLong:InvokeServerAsync() :: number
    local before: number = NetworkService:GetStats().Corrections
    if begin then
        begin()
    end
    local long = 0
    local last = os.clock()
    local watch = RunService.Heartbeat:Connect(function()
        local now = os.clock()
        if now - last > 0.05 then
            long += 1
        end
        last = now
    end)
    task.wait(seconds)
    watch:Disconnect()
    local after: number = NetworkService:GetStats().Corrections
    local serverAfter = serverLong:InvokeServerAsync() :: number
    return after - before, long + (serverAfter - serverBefore)
end

task.spawn(function()
    while NetworkService.State ~= Enum.NetworkState.Connected do
        task.wait()
    end
    local me = NetworkService.LocalPlayer :: Player
    while me.Character == nil do
        task.wait()
    end
    task.wait(2)
    -- A settled second, so the count starts from the walk and not the join.
    corrections(1)

    -- **The second in which the walk begins, counted apart** (D534). An input
    -- that changes is one intent, and when the packets that carry it are lost
    -- or late the authority has already stepped that tick with what the
    -- player was doing before -- standing -- so it starts one tick after the
    -- client did, and the client is corrected once, by one tick's walk. That
    -- is the link's doing and it is one correction: counted with the walk, it
    -- was "a straight walk corrected" some one run in fifty, with nothing to
    -- say why. The nine seconds after it are the straight walk.
    local start, startHitches = corrections(1, function()
        InputService:SetVirtualState(Enum.KeyCode.Virtual1, 1)
        if Condition.HangSeconds > 0 then
            task.delay(5, function()
                local began = os.clock()
                while os.clock() - began < Condition.HangSeconds do
                end
            end)
        end
    end)
    local walked, hitches = corrections(9)
    InputService:SetVirtualState(Enum.KeyCode.Virtual1, 0)
    task.wait(2)

    local where = Workspace:WaitForChild("Where") :: RemoteFunction
    -- A remote's round trip, as its median: one of ten that waited for a
    -- resend after a lost packet is the network's loss, measured elsewhere.
    local roundTrips: { number } = {}
    local stop: number = 0
    for _ = 1, 11 do
        local began = os.clock()
        local server = where:InvokeServerAsync() :: vector
        table.insert(roundTrips, (os.clock() - began) * 1000)
        local body = me.Character :: BasePart
        stop = math.max(stop, vector.magnitude(body.Position - server))
        task.wait(0.1)
    end
    table.sort(roundTrips)
    local remote: number = roundTrips[6]
    local stats = NetworkService:GetStats()
    local ping: number = stats.Ping
    local interpolation: number = stats.InterpolationDelay
    local counted: number = math.round(walked)
    local function decimal(value: number, places: number): string
        return string.format(`%.{places}f`, value)
    end
    print(
        `[acceptance] condition={Condition.Name} corrections={counted} hitches={hitches} `
            .. `start={math.round(start)} startHitches={startHitches} stop={decimal(stop, 4)} `
            .. `ping={decimal(ping, 1)} remote={decimal(remote, 1)} interpolation={decimal(interpolation, 1)}`
    )
end)
