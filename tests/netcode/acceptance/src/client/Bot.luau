--!strict
--[[
	The bot: once joined, it walks a straight line for ten seconds, stops,
	and measures -- the corrections its own character took while walking,
	how far the replica stopped from where the authority did, the ping, a
	remote's round trip, and how far in the past the others are drawn. One
	line, `[acceptance] ...`, which the gate's driver reads.

	`Condition` is written by the driver into each run's copy: the name of the
	condition, and a frame to hang in the middle of the walk, in seconds.

	**With a guest** (`Condition.Guest`, ADR 0208): the bot seats a second
	player at its own machine and walks both, each by its own intent, the two
	sticks pushed in the same tick. Everything above is then measured for each
	of the two, and one thing more: how many ticks each character took to
	answer its stick, which is the same for both or the guest is not predicted.
	And with `Condition.Nudge`, the authority is asked to put the guest's
	character a metre aside in the middle of the walk: the guest is corrected,
	and what is held is that the first player's character is not.
]]

local InputService = game:GetService("InputService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Condition = require("./Condition.module")
local Drive = require("@shared/Drive")

-- One player walks by a stick an `InputAction` reads. Two walk by intents
-- written in code (`BindToIntent`), each player's its own: there is one
-- virtual stick, and two people do not share one.
if not Condition.Guest then
    local context = Instance.new("InputContext")
    context.Parent = Workspace
    local move = Instance.new("InputAction")
    move.Name = "Move"
    move.Type = Enum.InputActionType.Direction2D
    move.Parent = context
    local stick = Instance.new("InputBinding")
    stick.KeyCode = Enum.KeyCode.VirtualStick1
    stick.Parent = move
end

if NetworkService.Authority then
    return
end

-- The client steps its own characters with the code the server steps them with.
RunService.Heartbeat:Connect(function()
    for _, player in NetworkService:GetLocalPlayers() do
        local body = player.Character
        if body and body:IsA("CharacterBody") then
            Drive.step(player, body :: CharacterBody)
        end
    end
end)

-- What the machine has counted, and of that what each of its players has.
type Count = { All: number, Of: { [number]: number } }
local function counted(): Count
    local stats = NetworkService:GetStats()
    local count: Count = { All = stats.Corrections, Of = {} }
    for _, player in stats.LocalPlayers do
        count.Of[player.UserId] = player.Corrections
    end
    return count
end

-- The corrections taken in `seconds` of simulated time, counted -- and the
-- frames in that time, on either machine, that took longer than three ticks,
-- which drop simulated time and may each cost a correction: the one this run
-- hangs on purpose, and any a busy machine makes of its own.
--
-- `begin`, when there is one, runs once the count has started: what is done in
-- it -- a key going down -- is inside the time measured.
local function corrections(seconds: number, begin: (() -> ())?): (Count, number)
    local serverLong = Workspace:WaitForChild("LongFrames") :: RemoteFunction
    local serverBefore = serverLong:InvokeServerAsync() :: number
    local before = counted()
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
    local after = counted()
    local serverAfter = serverLong:InvokeServerAsync() :: number
    local taken: Count = { All = after.All - before.All, Of = {} }
    for user, count in after.Of do
        taken.Of[user] = count - (before.Of[user] or 0)
    end
    return taken, long + (serverAfter - serverBefore)
end

local function decimal(value: number, places: number): string
    return string.format(`%.{places}f`, value)
end

task.spawn(function()
    while NetworkService.State ~= Enum.NetworkState.Connected do
        task.wait()
    end
    local me = NetworkService.LocalPlayer :: Player
    while me.Character == nil do
        task.wait()
    end
    local guest: Player? = nil
    local walking = false
    if Condition.Guest then
        local seated = NetworkService:AddLocalPlayer() :: Player
        guest = seated
        local function stick(intent: IntentWriter)
            intent:Set("Move", Vector2.new(0, if walking then 1 else 0))
        end
        RunService:BindToIntent("First", stick)
        RunService:BindToIntent("Guest", stick, seated)
        while seated.Character == nil do
            task.wait()
        end
    end
    task.wait(2)
    -- A settled second, so the count starts from the walk and not the join.
    corrections(1)

    -- **How many ticks each character takes to answer its stick** (ADR 0208):
    -- counted from the tick the sticks were pushed to the first in which the
    -- character is somewhere else.
    local answered: { [Player]: number } = {}
    local function watchSticks()
        local rest: { [Player]: vector } = {}
        for _, player in NetworkService:GetLocalPlayers() do
            rest[player] = (player.Character :: BasePart).Position
        end
        local ticks = 0
        local watch: Connection
        watch = RunService.Heartbeat:Connect(function()
            ticks += 1
            local waiting = false
            for player, was in rest do
                if answered[player] == nil then
                    local body = player.Character :: BasePart
                    if vector.magnitude(body.Position - was) > 0.01 then
                        answered[player] = ticks
                    else
                        waiting = true
                    end
                end
            end
            if not waiting or ticks > 120 then
                watch:Disconnect()
            end
        end)
    end

    -- **The second in which the walk begins, counted apart**. An input
    -- that changes is one intent, and when the packets that carry it are lost
    -- or late the authority has already stepped that tick with what the
    -- player was doing before -- standing -- so it starts one tick after the
    -- client did, and the client is corrected once, by one tick's walk. That
    -- is the link's doing and it is one correction: counted with the walk, it
    -- was "a straight walk corrected" some one run in fifty, with nothing to
    -- say why. The nine seconds after it are the straight walk.
    local start, startHitches = corrections(1, function()
        watchSticks()
        if Condition.Guest then
            walking = true
        else
            InputService:SetVirtualState(Enum.KeyCode.Virtual1, 1)
        end
        if Condition.HangSeconds > 0 then
            task.delay(5, function()
                local began = os.clock()
                while os.clock() - began < Condition.HangSeconds do
                end
            end)
        end
    end)

    -- **The authority's word a metre off for the guest alone**, three seconds
    -- into the walk: what the first player's character does in the second and
    -- a half round it -- each tick's step, and how far aside it ever is.
    local nudge = ""
    if Condition.Nudge and guest then
        local seated = guest
        task.delay(3, function()
            local body = me.Character :: BasePart
            local before = counted()
            local aside = body.Position.x
            -- From one tick's end to the next: the first one seen is where
            -- the measuring starts, not a step.
            local was: vector? = nil
            local least, most, drifted = math.huge, -math.huge, 0
            local watch = RunService.Heartbeat:Connect(function()
                local now = body.Position
                if was then
                    local step = vector.magnitude(now - was)
                    least = math.min(least, step)
                    most = math.max(most, step)
                end
                drifted = math.max(drifted, math.abs(now.x - aside))
                was = now
            end)
            local push = Workspace:WaitForChild("Nudge") :: RemoteFunction
            push:InvokeServerAsync(seated.UserId)
            task.wait(1.5)
            watch:Disconnect()
            local after = counted()
            local size = 0
            for _, player in NetworkService:GetStats().LocalPlayers do
                if player.UserId == seated.UserId then
                    size = player.LastCorrection
                end
            end
            nudge = ` nudgeGuest={(after.Of[seated.UserId] or 0) - (before.Of[seated.UserId] or 0)}`
                .. ` nudgeFirst={(after.Of[me.UserId] or 0) - (before.Of[me.UserId] or 0)}`
                .. ` nudgeSize={decimal(size, 3)}`
                .. ` aside={decimal(drifted, 6)} stepLeast={decimal(least, 4)} stepMost={decimal(most, 4)}`
        end)
    end

    local walked, hitches = corrections(9)
    if Condition.Guest then
        walking = false
    else
        InputService:SetVirtualState(Enum.KeyCode.Virtual1, 0)
    end
    task.wait(2)

    local where = Workspace:WaitForChild("Where") :: RemoteFunction
    -- A remote's round trip, as its median: one of ten that waited for a
    -- resend after a lost packet is the network's loss, measured elsewhere.
    local roundTrips: { number } = {}
    local stop: number = 0
    local guestStop: number = 0
    for _ = 1, 11 do
        local began = os.clock()
        local server = where:InvokeServerAsync() :: vector
        table.insert(roundTrips, (os.clock() - began) * 1000)
        local body = me.Character :: BasePart
        stop = math.max(stop, vector.magnitude(body.Position - server))
        if guest then
            local theirs = where:InvokeServerAsync(guest.UserId) :: vector
            local seat = guest.Character :: BasePart
            guestStop = math.max(guestStop, vector.magnitude(seat.Position - theirs))
        end
        task.wait(0.1)
    end
    table.sort(roundTrips)
    local remote: number = roundTrips[6]
    local stats = NetworkService:GetStats()
    local ping: number = stats.Ping
    local interpolation: number = stats.InterpolationDelay
    local line = `[acceptance] condition={Condition.Name} corrections={math.round(walked.All)} hitches={hitches} `
        .. `start={math.round(start.All)} startHitches={startHitches} stop={decimal(stop, 4)} `
        .. `ping={decimal(ping, 1)} remote={decimal(remote, 1)} interpolation={decimal(interpolation, 1)}`
    if guest then
        -- Each of the two by itself, the guest's last.
        line ..= ` firstCorrections={walked.Of[me.UserId] or -1} guestCorrections={walked.Of[guest.UserId] or -1}` .. ` firstStart={start.Of[me.UserId] or -1} guestStart={start.Of[guest.UserId] or -1}` .. ` guestStop={decimal(
            guestStop,
            4
        )}` .. ` firstStick={answered[me] or -1} guestStick={answered[guest] or -1}` .. nudge
    end
    print(line)
end)
