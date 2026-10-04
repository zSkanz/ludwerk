--!strict
--- Conformance: `Swarm`'s agents as a script hears and draws them (ADR 0162),
--- played solo.
---
--- **One script draws the horde solo, hosting and joined.** Solo, this machine
--- is the authority: the signals follow its own calls, and a body given to an
--- agent is moved by the swarm. What a replica is sent is the replication
--- module's own tests.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local PhysicsService = game:GetService("PhysicsService")

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
end

local function horde(): Swarm
    local swarm = Instance.new("Swarm")
    swarm.Target = vector.create(0, 0, -200)
    swarm.Parent = workspace
    return swarm
end

describe("a swarm's agents, heard", function()
    it("says who came, with its tag and its size, and who went, with the last tag and why", function()
        local swarm = horde()
        local added: { { any } } = {}
        local removed: { { any } } = {}
        swarm.AgentAdded:Connect(function(agent, tag, size)
            table.insert(added, { agent, tag, size })
        end)
        swarm.AgentRemoved:Connect(function(agent, tag, position, reason)
            table.insert(removed, { agent, tag, position, reason })
        end)

        local agent = swarm:AddAgentAt(vector.create(3, 0, 4), { Radius = 0.75, Height = 2, Speed = 0 })
        step(2)
        expect(#added):ToBe(1)
        expect(added[1][1]):ToBe(agent)
        expect(added[1][2]):ToBe(0)
        expect(added[1][3]):ToBe(vector.create(1.5, 2, 1.5))

        -- A "killed" bit and the removal in one tick: the bit is in the removal.
        swarm:SetAgentTag(agent, 0x8001)
        swarm:RemoveAgent(agent)
        step(2)
        expect(#removed):ToBe(1)
        expect(removed[1][1]):ToBe(agent)
        expect(removed[1][2]):ToBe(0x8001)
        expect(removed[1][3]):ToBe(vector.create(3, 0, 4))
        expect(removed[1][4]):ToBe(Enum.SwarmAgentRemoval.Removed)
        swarm:Destroy()
    end)

    it("lists its agents and their places into a table of the caller's own", function()
        local swarm = horde()
        local first = swarm:AddAgentAt(vector.create(1, 0, 0), { Speed = 0 })
        local second = swarm:AddAgentAt(vector.create(2, 0, 0), { Speed = 0 })
        local numbers = { 7, 7, 7, 7 }
        local places: { vector } = {}
        expect(swarm:GetAgents(numbers)):ToBe(numbers)
        expect(swarm:GetPositions(places)):ToBe(places)
        expect(#numbers):ToBe(2)
        expect(numbers[1]):ToBe(first)
        expect(numbers[2]):ToBe(second)
        expect(#places):ToBe(2)
        expect(places[2].x):ToBe(2)
        -- And a new one each time when none is given, as it always was.
        expect(swarm:GetAgents() ~= swarm:GetAgents()):ToBe(true)
        expect(#swarm:GetPositions()):ToBe(2)
        swarm:Destroy()
    end)

    it("carries a tag for each agent, from 0 to 65535", function()
        local swarm = horde()
        local heard: { { number } } = {}
        swarm.AgentTagChanged:Connect(function(agent, tag)
            table.insert(heard, { agent, tag })
        end)
        local agent = swarm:AddAgentAt(vector.create(0, 0, 0), { Speed = 0 })
        expect(swarm:GetAgentTag(agent)):ToBe(0)
        swarm:SetAgentTag(agent, 513)
        expect(swarm:GetAgentTag(agent)):ToBe(513)
        -- The same tag again is nothing new.
        swarm:SetAgentTag(agent, 513)
        step(2)
        expect(#heard):ToBe(1)
        expect(heard[1][1]):ToBe(agent)
        expect(heard[1][2]):ToBe(513)

        local anySwarm = swarm :: any
        expect(function()
            anySwarm:SetAgentTag(agent, 65536)
        end):ToThrow()
        expect(function()
            anySwarm:SetAgentTag(agent, -1)
        end):ToThrow()
        expect(function()
            anySwarm:SetAgentTag(agent, 1.5)
        end):ToThrow()
        -- An agent that is not there has no tag.
        expect(swarm:GetAgentTag(999)):ToBe(0)
        swarm:Destroy()
    end)

    it("moves the body an agent is given, and leaves it when it is let go", function()
        local swarm = horde()
        local agent = swarm:AddAgentAt(vector.create(0, 0, 0), { Speed = 6 })
        local body = Instance.new("Part")
        body.Anchored = true
        body.CanCollide = false
        body.Parent = workspace
        swarm:SetAgentBody(agent, body)
        step(30)
        local where = swarm:GetAgentPosition(agent) :: vector
        expect(where.z < -2):ToBe(true)
        expect(math.abs(body.Position.z - where.z) < 0.2):ToBe(true)

        swarm:SetAgentBody(agent, nil)
        local left = body.Position
        step(30)
        expect(body.Position):ToBe(left)
        expect((swarm:GetAgentPosition(agent) :: vector).z < where.z - 2):ToBe(true)

        expect(function()
            (swarm :: any):SetAgentBody(agent, workspace)
        end):ToThrow()
        body:Destroy()
        swarm:Destroy()
    end)

    it("is not replicated until asked, and says how far it reaches when it is", function()
        local swarm = horde()
        expect(swarm.Replicates):ToBe(false)
        expect(swarm.ReplicationRadius):ToBe(80)
        swarm.Replicates = true
        swarm.ReplicationRadius = 120
        expect(swarm.Replicates):ToBe(true)
        expect(swarm.ReplicationRadius):ToBe(120)
        expect(function()
            swarm.ReplicationRadius = 0
        end):ToThrow()
        swarm:Destroy()
    end)
end)

describe("Player.ReplicationFocus", function()
    it("is a part, or nothing", function()
        local player = game:GetService("NetworkService").LocalPlayer :: Player
        expect(player.ReplicationFocus == nil):ToBe(true)
        local part = Instance.new("Part")
        part.Parent = workspace
        player.ReplicationFocus = part
        expect(player.ReplicationFocus):ToBe(part)
        expect(function()
            (player :: any).ReplicationFocus = workspace
        end):ToThrow()
        player.ReplicationFocus = nil
        expect(player.ReplicationFocus == nil):ToBe(true)
        part:Destroy()
    end)
end)

describe("Player.Character", function()
    it("is taken away by nil", function()
        local player = game:GetService("NetworkService").LocalPlayer :: Player
        local before = player.Character
        local part = Instance.new("Part")
        part.Parent = workspace
        player.Character = part
        expect(player.Character):ToBe(part)
        -- D538: nil was refused, so a script could give a character and never take it away.
        player.Character = nil
        expect(player.Character == nil):ToBe(true)
        player.Character = before
        part:Destroy()
    end)
end)
