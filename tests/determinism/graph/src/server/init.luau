--!strict
--- An animation graph over fifteen seconds (ADR 0197): a character that
--- stands, walks, runs, jumps and is made to attack, with nothing playing a
--- clip -- its `AnimationPlayer` names a graph, and the graph reads the body.
---
--- **What the trace holds is the graph's own state**, tick by tick: which
--- state each layer is in, how far through, what it is fading from and what
--- its parameters are, as the one number a player carries where the world's
--- hash reads it. A graph that picked another state on another run, or took
--- a transition a tick later, moves the hash at the checkpoint after.
---
--- The content is the two-process animation gate's (`tests/netcode/animated`),
--- copied: a block of a body, six clips told apart by their lengths, and the
--- graph. The assertions are `error` calls, which fail a replay.

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(400, 1, 400)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = Workspace

-- Made in code: a replay's world is given nowhere to read a stamp from.
local hero = Instance.new("CharacterBody")
hero.Name = "Hero"
hero.Size = vector.create(1, 2, 1)
hero.JumpSpeed = 9
hero.Position = vector.create(0, 1.5, 0)

local body = Instance.new("MeshPart")
body.Name = "Body"
body.MeshContent = "asset://models/walker.gltf"
body.Anchored = true
body.CanCollide = false
body.Parent = hero

local animator = Instance.new("AnimationPlayer")
animator.Graph = "asset://anim/walker.animgraph.json"
animator.Parent = hero
hero.Parent = Workspace

local Forward = vector.create(0, 0, -1)
local did: { string } = {}
local steps = 0
local hits = 0
local elapsed = 0
local jumped = false
local attacked = false
local judged = false

animator.StateChanged:Connect(function(layer: string, from: string, to: string)
    -- From the walk on: before it the character is settling onto the floor.
    if elapsed >= 2 then
        table.insert(did, `{layer}:{from}>{to}`)
    end
end)
animator.EventReached:Connect(function(name: string)
    if name == "Step" then
        steps += 1
    elseif name == "Hit" then
        hits += 1
    end
end)

local function near(value: number, wanted: number, within: number, what: string)
    if math.abs(value - wanted) > within then
        error(`graph: {what} is {value}, and {wanted} was expected`)
    end
end

RunService.Heartbeat:Connect(function(dt: number)
    elapsed += dt
    if elapsed < 2 then
        hero:Move(vector.zero)
    elseif elapsed < 5 then
        hero.WalkSpeed = 3
        hero:Move(Forward)
        if elapsed > 4 then
            near(animator:GetParameter("Speed") :: number, 3, 0.1, "the speed the graph reads of a walk")
        end
    elseif elapsed < 8 then
        hero.WalkSpeed = 8
        hero:Move(Forward)
        if elapsed > 7 then
            near(animator:GetParameter("Speed") :: number, 8, 0.2, "the speed the graph reads of a run")
        end
    elseif elapsed < 11 then
        hero:Move(vector.zero)
        if not jumped and elapsed > 8.5 then
            jumped = true
            hero:Jump()
        end
    elseif elapsed < 13 then
        hero:Move(vector.zero)
        if not attacked and elapsed > 11.25 then
            attacked = true
            hero:SetAttribute("Attack", 1)
        end
    elseif not judged then
        judged = true
        hero:Move(vector.zero)
        local wanted = "Body:Move>Jump Body:Jump>Fall Body:Fall>Move Arms:None>Slash Arms:Slash>None"
        local got = table.concat(did, " ")
        if got ~= wanted then
            error(`graph: its states went "{got}", and "{wanted}" was expected`)
        end
        -- A walk of three seconds and a run of three, a step a cycle each:
        -- three of the walk's second-long cycles and six of the run's halves,
        -- give or take the one the blend between them shares.
        if steps < 7 or steps > 11 then
            error(`graph: {steps} steps were heard over a walk and a run`)
        end
        if hits ~= 1 then
            error(`graph: the slash hit {hits} times`)
        end
        if animator:GetState() ~= "Move" or animator:GetState("Arms") ~= "None" then
            error(`graph: it ended in {animator:GetState()} and {animator:GetState("Arms")}`)
        end
    else
        hero:Move(vector.zero)
    end
end)
