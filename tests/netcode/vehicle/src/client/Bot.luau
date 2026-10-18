--!strict
--[[
	The bot: once joined, it drives its car -- a straight line, a corner, a
	stop on the brake -- and measures the corrections its car took in each,
	the furthest any of them moved it, how far its car stopped from where the
	authority's did, and how many ticks its car and its character each took to
	answer their sticks, pushed in the same tick. One line, `[vehicle] ...`,
	which the gate's driver reads.

	The car is driven by intents written in code (`BindToIntent`): `Drive`
	and `Brake` are what the server's `VehicleController.Driver` reads, on
	the authority and here alike, and `Move` walks the character.

	**Into a parked car** (`park`): the authority parks a car twenty metres
	ahead, and the bot drives into it at a walking pace and stops -- the
	corrections of two cars touching, and the stop after.

	**Nudged** (`nudge`): as a drive, but in the straight the authority moves
	the car half a metre aside, once. Nothing here predicts that: it is a
	correction, and the line has to say so and how big -- or the measure is
	blind.

	**Two drivers** (`duel`): two players, their cars facing each other, each
	driving at the other with a wheel that wanders from a seed of its own.
	This machine cannot know where the other's wheel goes next: corrections
	are expected while they touch, and what is measured is their size.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local Condition = require("@shared/Condition")

if NetworkService.Authority then
    return
end

-- What the bot asks for, each tick.
local throttle, steer, braking, walking = 0, 0, false, false
RunService:BindToIntent("Bot", function(intent: IntentWriter)
    intent:Set("Drive", Vector2.new(steer, throttle))
    intent:Set("Brake", braking)
    intent:Set("Move", Vector2.new(0, if walking then 1 else 0))
end)

-- The character walks here as it does on the authority.
RunService.Heartbeat:Connect(function()
    local me = NetworkService.LocalPlayer
    local body = if me then me.Character else nil
    if me and body and body:IsA("CharacterBody") then
        local move = me:GetIntent("Move")
        local walk = if typeof(move) == "Vector2" then vector.create(move.X, 0, -move.Y) else vector.zero
        local walker = body :: CharacterBody
        walker:Move(walk)
    end
end)

-- The furthest any correction moved what this machine predicts, in metres,
-- inside the measured stretches: what a correction looks like when there is
-- one.
local largest = 0

local function corrections(seconds: number): (number, number)
    local serverLong = Workspace:WaitForChild("LongFrames") :: RemoteFunction
    local serverBefore = serverLong:InvokeServerAsync() :: number
    local before = NetworkService:GetStats().Corrections
    local seen = before
    local long = 0
    local last = os.clock()
    local watch = RunService.Heartbeat:Connect(function()
        local now = os.clock()
        if now - last > 0.05 then
            long += 1
        end
        last = now
        local stats = NetworkService:GetStats()
        if stats.Corrections > seen then
            seen = stats.Corrections
            largest = math.max(largest, stats.LastCorrection)
        end
    end)
    task.wait(seconds)
    watch:Disconnect()
    local serverAfter = serverLong:InvokeServerAsync() :: number
    return NetworkService:GetStats().Corrections - before, long + (serverAfter - serverBefore)
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
    local car = Workspace:WaitForChild(`Car{me.UserId}`) :: Part
    local controller = car:WaitForChild("VehicleController") :: VehicleController
    -- Whether this machine knows it drives: what puts the car in its island.
    local driven = false
    for _ = 1, if Condition.Mode == "late" then 600 else 120 do
        if controller.Driver == me then
            driven = true
            break
        end
        task.wait()
    end
    -- The other car, for a run that has one: parked, or the other player's.
    local other: Part? = nil
    if Condition.Mode == "park" then
        local park = Workspace:WaitForChild("Park") :: RemoteFunction
        park:InvokeServerAsync()
        other = Workspace:WaitForChild("Parked") :: Part
    elseif Condition.Mode == "duel" then
        -- Both drivers in, before either sets off.
        while #NetworkService:GetPlayers() < 2 do
            task.wait()
        end
        for _, player in NetworkService:GetPlayers() do
            if player ~= me then
                other = Workspace:WaitForChild(`Car{player.UserId}`) :: Part
            end
        end
    end
    task.wait(2)
    corrections(1)
    -- What arriving cost is not what driving does.
    largest = 0

    -- **How many ticks each answers its stick in**: the car and the
    -- character, pushed in the same tick, counted to the first tick each is
    -- somewhere else.
    -- The authority starts watching its own car's jumps first: asking takes
    -- a round trip, which is not to be counted in the ticks below.
    local jumps = Workspace:WaitForChild("Jumps") :: RemoteFunction
    jumps:InvokeServerAsync(true)
    -- Where the server's long frames stand as the drive begins: asked here,
    -- before the answer's ticks are counted, so the round trip is in none.
    local serverLong = Workspace:WaitForChild("LongFrames") :: RemoteFunction
    local serverLongAtStart = serverLong:InvokeServerAsync() :: number

    -- The car answers when its engine does -- its turns change -- or it
    -- moves: from a standstill an automatic gearbox puts it in gear and lets
    -- the clutch in over a few ticks, which is the car's and not the link's.
    local carTicks, walkTicks = -1, -1
    local carWas = car.Position
    local rpmWas = controller.RPM
    local body = me.Character :: BasePart
    local walkWas = body.Position
    local ticks = 0
    local watch: Connection
    watch = RunService.Heartbeat:Connect(function()
        ticks += 1
        if
            carTicks < 0 and (vector.magnitude(car.Position - carWas) > 0.0001 or math.abs(controller.RPM - rpmWas) > 5)
        then
            carTicks = ticks
        end
        if walkTicks < 0 and vector.magnitude(body.Position - walkWas) > 0.0001 then
            walkTicks = ticks
        end
        if (carTicks >= 0 and walkTicks >= 0) or ticks > 120 then
            watch:Disconnect()
        end
    end)

    -- **What a correction looks like on the screen**: the car's place a
    -- frame, drawn (`GetRenderCFrame`, in a render step) and simulated, and
    -- the most either moved off the line its last two frames drew -- its
    -- second difference. A car driven smoothly is well under a millimetre;
    -- one that snaps is the snap.
    local drawnJump, simJump = 0, 0
    local drawn: { vector } = {}
    local simulated: { vector } = {}
    --
    -- **Off the line its last two frames drew, at this frame's length**: a
    -- frame drawn after a long one and before a short one puts a car moving
    -- steadily at unevenly spaced places, which is no jump -- on a machine
    -- under load it was seven centimetres of one at 300 ms.
    local spans: { number } = {}
    local function jumpOf(history: { vector }, at: vector, span: number): number
        table.insert(history, at)
        table.insert(spans, span)
        if #history > 3 then
            table.remove(history, 1)
        end
        if #spans > 3 then
            table.remove(spans, 1)
        end
        if #history < 3 or spans[2] <= 0 then
            return 0
        end
        local expected = history[2] + (history[2] - history[1]) * (spans[3] / spans[2])
        return vector.magnitude(history[3] - expected)
    end
    -- **How far the car is from the authority's**, at every answer
    -- (`PredictedDrift`): left a few centimetres under a correction and
    -- pulled back softly, it stays small; one that is never pulled back
    -- would sit just under the correction for as long as it drives.
    local driftSum, driftCount, driftMax = 0, 0, 0
    -- **The car's own**: the drift is the furthest predicted part's, and
    -- once two cars touch the other is one of them. Its drift is kept apart,
    -- by name. And the car's drift in the ticks no correction answered: a
    -- correction fires at `VehicleCorrectionMetres` by design, so the tick it
    -- fires reads at least that much. **And the ticks after it read it
    -- again**: the drift is the last answer's, held until the next one
    -- comes, so the ticks between a correction and the next answer read the
    -- very drift that correction answered -- 32.4 mm, after a 0.032 m
    -- correction of it, was taken for drift nothing answered.
    local driftBetween = 0
    local answered = -1
    local otherDrift, otherPart = 0, "--"
    -- **Every correction of the drive, and the largest**, not only the ones
    -- inside the stretches `corrections` counts: an 11 cm snap between two
    -- of them was reported as none at all.
    local runCorrections, runLargest = 0, 0
    local runSeen = NetworkService:GetStats().Corrections
    -- **And every long frame of the drive**, both machines', for the same
    -- reason: a frame of 200 ms between two stretches was in no row. The
    -- server's count was asked before the ticks to answer were counted.
    local runLong = 0
    local lastFrame = os.clock()
    -- **What the stall was, as this machine's clock saw it**: the frame it
    -- made, the frame before it, and the ticks the frames after it ran. A
    -- client catches up a stall once, to a quarter of a second, and not one
    -- that follows a frame long enough to owe more than its four ticks
    -- (`StallCatchUpSeconds`, `FrameScheduler`); what it does not catch up it
    -- drops. Frames are counted at `PreRender`, ticks at `Heartbeat`.
    local stallDone = false
    local stallSeen = 0
    local stallFrame, stallBefore, stallTicks = 0, 0, 0
    local frameAt = os.clock()
    local lastFrameLength = 0
    local ticksThisFrame = 0
    local framing = RunService.PreRender:Connect(function()
        local now = os.clock()
        local length = now - frameAt
        frameAt = now
        if stallDone and stallSeen < 3 then
            stallSeen += 1
            stallFrame = math.max(stallFrame, length)
            stallTicks = math.max(stallTicks, ticksThisFrame)
        end
        lastFrameLength = length
        ticksThisFrame = 0
    end)
    local simulatedSpans: { number } = {}
    local function simulatedJumpOf(at: vector): number
        -- The simulated place moves a tick at a time: its line is a tick long.
        local saved = spans
        spans = simulatedSpans
        local jump = jumpOf(simulated, at, 1)
        spans = saved
        return jump
    end
    -- **Counted in ticks, never in frames** (tests/editorshots/README.md):
    -- the simulated place and the drift are read at the end of every tick,
    -- as the authority reads its own car, so how many frames the machine
    -- drew between two ticks changes nothing.
    local tickLength = 1 / 60
    local ticking = RunService.Heartbeat:Connect(function(dt: number)
        tickLength = dt
        ticksThisFrame += 1
        simJump = math.max(simJump, simulatedJumpOf(car.Position))
        local frameNow = os.clock()
        if frameNow - lastFrame > 0.05 then
            runLong += 1
        end
        lastFrame = frameNow
        local stats = NetworkService:GetStats()
        local corrected = stats.Corrections > runSeen
        if corrected then
            runCorrections += stats.Corrections - runSeen
            runSeen = stats.Corrections
            runLargest = math.max(runLargest, stats.LastCorrection)
        end
        local drift = stats.PredictedDrift
        local part = stats.PredictedDriftPart
        if part == nil or part == (car :: BasePart) then
            driftSum += drift
            driftCount += 1
            driftMax = math.max(driftMax, drift)
            if corrected then
                answered = drift
            elseif drift ~= answered then
                driftBetween = math.max(driftBetween, drift)
            end
        elseif drift > otherDrift then
            otherDrift = drift
            otherPart = (part :: BasePart).Name
        end
    end)
    -- What is drawn can only be read in a frame. A frame that covers several
    -- ticks moves the car several ticks' worth off its line -- a stop in one
    -- tick is that stop times the frame's ticks -- so the jump is a tick's
    -- share of it, which a machine under load and one drawing every tick
    -- both measure the same.
    RunService:BindToRenderStep("CarJumps", 1, function(span: number)
        local ticks = math.max(1, span / tickLength)
        drawnJump = math.max(drawnJump, jumpOf(drawn, car:GetRenderCFrame().Position, span) / ticks)
    end)

    local start, straight, corner, stopping, contact, hitches = 0, 0, 0, 0, 0, 0
    local closest = math.huge
    local h = 0
    if other ~= nil then
        local them = other :: Part
        throttle = if Condition.Mode == "duel" then 0.6 else 0.35
        walking = true
        start, h = corrections(1)
        hitches += h
        walking = false
        -- How close the two came, centre to centre; and, for two drivers, a
        -- wheel that aims at the other car and wanders off it by up to half
        -- a turn, picked again every quarter second from a seed of this
        -- player's own -- so the two meet, and keep shoving, and neither
        -- machine knows where the other's wheel goes next.
        local wander = Random.new(me.UserId * 7919)
        local nextTurn = 0
        local off = 0
        local meeting = RunService.Heartbeat:Connect(function()
            closest = math.min(closest, vector.magnitude(them.Position - car.Position))
            if Condition.Mode == "duel" then
                if os.clock() >= nextTurn then
                    off = wander:NextNumber(-0.5, 0.5)
                    nextTurn = os.clock() + 0.25
                end
                local toward = vector.normalize(them.Position - car.Position)
                local aim = vector.dot(car.CFrame.RightVector, toward) * 2
                steer = math.clamp(aim + off, -1, 1)
            end
        end)
        contact, h = corrections(7)
        meeting:Disconnect()
        steer = 0
        hitches += h
    else
        throttle = 1
        walking = true
        start, h = corrections(1)
        hitches += h
        walking = false
        if Condition.Mode == "nudge" then
            -- Half a second into the straight, the authority moves the car.
            task.delay(0.5, function()
                local nudge = Workspace:WaitForChild("Nudge") :: RemoteFunction
                nudge:InvokeServerAsync()
            end)
        elseif Condition.Mode == "stall" or Condition.Mode == "stalllong" then
            -- A second into the straight, this machine stops: for 120 ms --
            -- a window dragged, a disk read, an alt-tab -- well inside the
            -- quarter second a client catches up; or for 400 ms, past it.
            local busy = if Condition.Mode == "stall" then 0.12 else 0.4
            task.delay(1, function()
                stallBefore = lastFrameLength
                local from = os.clock()
                while os.clock() - from < busy do
                end
                stallDone = true
            end)
        end
        straight, h = corrections(3)
        hitches += h
        steer = 0.6
        throttle = 0.6
        corner, h = corrections(3)
        hitches += h
        steer = 0
    end
    throttle = 0
    braking = true
    stopping, h = corrections(3)
    hitches += h
    RunService:UnbindFromRenderStep("CarJumps")
    ticking:Disconnect()
    framing:Disconnect()
    local authorityJump = jumps:InvokeServerAsync(false) :: number
    local serverLongAtEnd = serverLong:InvokeServerAsync() :: number
    -- The drive's long frames, both machines', in place of the stretches'
    -- own: what the rules allow for a long frame, they allow wherever it fell.
    hitches = math.max(hitches, runLong + (serverLongAtEnd - serverLongAtStart))
    task.wait(1)
    -- **Where the authority's car is, at the same tick**: the engine's own
    -- measure (`PredictedDrift`, this car against the authority's at the tick
    -- an answer is for). Its place now against the authority's place answered
    -- a round trip later measured a car still moving as one stopped apart --
    -- 4.6 mm at 1.7 cm/s and 300 ms, 12.3 mm once -- while the two at one
    -- tick were never more than 3 mm apart.
    local stop = 0
    for _ = 1, 11 do
        stop = math.max(stop, NetworkService:GetStats().PredictedDrift)
        task.wait(0.1)
    end
    braking = false
    local stats = NetworkService:GetStats()
    print(
        `[vehicle] condition={Condition.Name} seat={me.UserId} start={start} straight={straight} corner={corner} `
            .. `stopping={stopping} contact={contact} hitches={hitches} stop={decimal(stop, 4)} `
            .. `carTicks={carTicks} walkTicks={walkTicks} ping={decimal(stats.Ping, 1)} `
            .. `predicted={stats.PredictedParts} driver={driven} closest={decimal(closest, 3)} `
            .. `largest={decimal(runLargest, 3)} largestInStretches={decimal(largest, 3)} `
            .. `corrections={runCorrections} drawnJump={decimal(drawnJump * 1000, 1)} `
            .. `simJump={decimal(simJump * 1000, 1)} driftMean={decimal(driftSum / math.max(driftCount, 1) * 1000, 1)} `
            .. `driftMax={decimal(driftMax * 1000, 1)} driftBetween={decimal(driftBetween * 1000, 1)} `
            .. `otherDrift={decimal(otherDrift * 1000, 1)} otherPart={otherPart} car={car.Name} `
            .. `authorityJump={decimal(authorityJump * 1000, 1)} `
            .. `stallFrame={decimal(stallFrame * 1000, 1)} stallBefore={decimal(stallBefore * 1000, 1)} `
            .. `stallTicks={stallTicks}`
    )
end)
