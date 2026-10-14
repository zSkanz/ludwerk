--!strict
--- A sequence played, skipped, looped and sought over twenty-five seconds
--- (ADR 0204), with the simulation on the other end of every track.
---
--- **A sequence's clock is the tick's**, and what its tracks do to the world
--- is done in the tick: a character carried across a floor and set down to
--- stand by itself, a lift keyed up under two crates that ride it, a lamp,
--- and events whose handlers make parts. All of it is in the hash, so a build
--- in which any of it followed a frame, a wall clock or an order no container
--- promises moves the trace.
---
--- The camera track is there and is no part of this: it is picture, and a
--- replay has none.
---
--- The assertions are `error` calls, which fail a replay.

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local ticks = 0
local marks = 0

-- A part at a place the tick count decides: when something was said is in
-- the world, and so in its hash.
local function mark(what: string)
    marks += 1
    local part = Instance.new("Part")
    part.Name = `{what}{marks}`
    part.Anchored = true
    part.Position = vector.create(ticks * 0.01, 20 + marks, -10)
    part.Parent = Workspace
end

local function block(name: string, position: vector, size: vector, anchored: boolean): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part.Anchored = anchored
    part.Parent = Workspace
    return part
end

block("Floor", vector.create(0, -0.5, 0), vector.create(60, 1, 60), true)
local lift = block("Lift", vector.create(0, 0.5, 6), vector.create(4, 1, 4), true)
-- Two crates on the lift: carried up by a part the sequence moves.
block("CrateA", vector.create(-0.8, 1.6, 6), vector.create(1, 1, 1), false)
block("CrateB", vector.create(0.8, 1.6, 6), vector.create(1, 1, 1), false)
-- And one in the hero's way.
block("Obstacle", vector.create(2, 1, 0), vector.create(1, 2, 1), false)

local lamp = block("Lamp", vector.create(0, 8, 0), vector.create(1, 1, 1), true)
local light = Instance.new("PointLight")
light.Name = "Light"
light.Parent = lamp

local hero = Instance.new("CharacterBody")
hero.Name = "Hero"
hero.Size = vector.create(2, 5, 2)
hero.Position = vector.create(-6, 3, 0)
hero.Parent = Workspace

local function player(name: string): SequencePlayer
    local made = Instance.new("SequencePlayer")
    made.Name = name
    made.Sequence = "asset://sequences/walk.sequence.json" :: Content
    for bound, target in { Hero = hero, Lift = lift, Lamp = lamp } :: { [string]: Instance } do
        local binding = Instance.new("SequenceBinding")
        binding.Name = bound
        binding.Target = target
        binding.Parent = made
    end
    made.Parent = Workspace
    return made
end

local scene = player("Scene")
local heard: { string } = {}
scene.EventReached:Connect(function(name: string, _data: any, skipped: boolean)
    table.insert(heard, if skipped then `{name}!` else name)
    mark(name)
end)
local finished = 0
local skippedOnce = false
scene.Finished:Connect(function(skipped: boolean)
    finished += 1
    skippedOnce = skippedOnce or skipped
    mark("Finished")
end)

-- A second one that goes round, at half speed: its events are all it does,
-- and they land on ticks too.
local round = Instance.new("SequencePlayer")
round.Name = "Round"
round.Sequence = "asset://sequences/round.sequence.json" :: Content
round.Looped = true
round.Speed = 0.5
round.Parent = Workspace
local rounds = 0
round.EventReached:Connect(function(name: string)
    if name == "Begin" then
        rounds += 1
        mark("Round")
    end
end)

local function expectNear(what: string, got: number, wanted: number, within: number)
    if math.abs(got - wanted) > within then
        error(`{what} is {got}, not {wanted}`)
    end
end

RunService.Heartbeat:Connect(function()
    ticks += 1
    if ticks == 5 then
        scene:Play()
        round:Play()
    end
    if ticks == 120 then
        -- A second and three quarters in: carried, held, part of the way.
        if not hero.Anchored then
            error("the hero is not held while its place is keyed")
        end
        if hero.Position.x < -5 or hero.Position.x > 3 then
            error(`the hero is at {hero.Position.x} a second and three quarters in`)
        end
    end
    if ticks == 240 then
        -- Its track ran out at three seconds: set down, its own again.
        if hero.Anchored then
            error("the hero was not let go when its track ran out")
        end
        expectNear("where the hero was set down", hero.Position.x, 6, 0.5)
    end
    if ticks == 400 then
        -- Over, by itself, six seconds after it began.
        if finished ~= 1 or skippedOnce then
            error(`finished {finished} time(s), skipped={skippedOnce}`)
        end
        if table.concat(heard, " ") ~= "Begin Middle Late" then
            error(`heard: {table.concat(heard, " ")}`)
        end
        expectNear("the lamp at the end", light.Brightness, 6, 0.001)
        expectNear("the lift at the end", lift.Position.y, 5, 0.001)
        -- The hero stands on the floor by itself now.
        expectNear("the hero's height, standing", hero.Position.y, 2.5, 0.2)
    end
    if ticks == 420 then
        -- Again from the start, and skipped a second in.
        table.clear(heard)
        scene.TimePosition = 0
        scene:Play()
    end
    if ticks == 480 then
        scene:Skip()
    end
    if ticks == 490 then
        if finished ~= 2 or not skippedOnce then
            error(`after the skip: finished {finished} time(s), skipped={skippedOnce}`)
        end
        if table.concat(heard, " ") ~= "Begin Middle! Late!" then
            error(`heard across the skip: {table.concat(heard, " ")}`)
        end
        -- Where the sequence would have left it: the hero at its last key,
        -- let go; the lift up; the lamp up.
        if hero.Anchored then
            error("a skip left the hero held")
        end
        expectNear("the hero after the skip", hero.Position.x, 6, 0.5)
        expectNear("the lift after the skip", lift.Position.y, 5, 0.001)
        expectNear("the lamp after the skip", light.Brightness, 6, 0.001)
    end
    if ticks == 700 then
        -- Sought, held still: every track at two seconds, and nothing said.
        table.clear(heard)
        scene.TimePosition = 2
    end
    if ticks == 705 then
        if #heard ~= 0 then
            error(`a seek fired {table.concat(heard, " ")}`)
        end
        expectNear("the lift at two seconds", lift.Position.y, 1.625, 0.01)
        scene:Play()
    end
    if ticks == 760 then
        scene:Stop()
    end
    if ticks == 770 then
        if scene.TimePosition ~= 0 or finished ~= 2 then
            error(`a stop left it at {scene.TimePosition} having finished {finished} time(s)`)
        end
        if hero.Anchored then
            error("a stop left the hero held")
        end
    end
    if ticks == 1490 then
        -- Half speed, six seconds long: round every twelve seconds, and a
        -- little into the third.
        if rounds ~= 3 then
            error(`the looped one began {rounds} time(s) in twenty-five seconds, not 3`)
        end
    end
end)
