--!strict
--- An `AnimationPlayer` with a `Graph` (ADR 0197): what a script sets, reads
--- and hears.
---
--- **These cases run against a rig that is not there**, as the ones beside
--- them do: the suite is headless and loads no glTF, so the graph's clips have
--- no length and move nothing. Its states, its conditions and its triggers do
--- not need them, and that is what is held here -- with the state a real game
--- is in before the graph's own file has arrived, where a parameter set must
--- be kept and a read must not be a crash. What a graph does to a pose is
--- asserted in C++ (`engine_render_tests`: `graph_player_tests.cpp`,
--- `animation_graph_tests.cpp`).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local beforeEach, afterEach = testing.beforeEach, testing.afterEach

local PhysicsService = game:GetService("PhysicsService")

local Graph = "asset://anim/spec.animgraph.json"

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
end

describe("AnimationPlayer with a graph", function()
    local mesh: MeshPart = nil :: any
    local player: AnimationPlayer = nil :: any

    beforeEach(function()
        mesh = Instance.new("MeshPart")
        mesh.Name = "GraphSpecMesh"
        mesh.Anchored = true
        mesh.Parent = workspace
        player = Instance.new("AnimationPlayer")
        player.Parent = mesh
    end)

    afterEach(function()
        mesh:Destroy()
    end)

    it("has no graph until it is given one, and says nothing of one", function()
        expect(player.Graph):ToBe("")
        expect(player.Retargeting):ToBe(Enum.Retargeting.Automatic)
        expect(player:GetState()):ToBe("")
        expect(player:GetParameter("Speed")):ToBe(nil)
        -- A write with no graph is kept for one, not refused.
        player:SetParameter("Speed", 3)
        player:ClearParameter("Speed")
    end)

    it("starts each layer in its first state once its file has arrived", function()
        player.Graph = Graph
        expect(player.Graph):ToBe(Graph)
        step(3)
        expect(player:GetState()):ToBe("Idle")
        expect(player:GetState("Body")):ToBe("Idle")
        expect(player:GetState("Arms")):ToBe("None")
        -- A layer the graph does not have has no state.
        expect(player:GetState("Legs")):ToBe("")
    end)

    it("reads each kind of parameter as what it is", function()
        player.Graph = Graph
        step(3)
        expect(player:GetParameter("Speed")):ToBe(0)
        expect(player:GetParameter("Armed")):ToBe(false)
        expect(player:GetParameter("Attack")):ToBe(false)
        expect(player:GetParameter("Mood")):ToBe(2)
        expect(player:GetParameter("NoSuch")):ToBe(nil)
    end)

    it("keeps what was set before the graph arrived", function()
        player.Graph = Graph
        player:SetParameter("Speed", 4)
        step(3)
        expect(player:GetParameter("Speed")):ToBe(4)
        expect(player:GetState()):ToBe("Run")
    end)

    it("moves between states as its parameters say, and says so", function()
        player.Graph = Graph
        step(3)
        local heard: { string } = {}
        player.StateChanged:Connect(function(layer: string, from: string, to: string)
            table.insert(heard, `{layer}:{from}>{to}`)
        end)
        player:SetParameter("Speed", 5)
        step(3)
        expect(player:GetState()):ToBe("Run")
        player:SetParameter("Speed", 0)
        step(3)
        expect(player:GetState()):ToBe("Idle")
        expect(#heard):ToBe(2)
        expect(heard[1]):ToBe("Body:Idle>Run")
        expect(heard[2]):ToBe("Body:Run>Idle")
    end)

    it("fires a trigger once, and only where every condition beside it holds", function()
        player.Graph = Graph
        step(3)
        -- Unarmed: the trigger is spent on the tick it was fired in.
        player:SetParameter("Attack", true)
        step(3)
        expect(player:GetState("Arms")):ToBe("None")
        expect(player:GetParameter("Attack")):ToBe(false)
        -- Armed: it is taken.
        player:SetParameter("Armed", true)
        player:SetParameter("Attack", true)
        step(3)
        expect(player:GetState("Arms")):ToBe("Slash")
        player:SetParameter("Armed", false)
        step(3)
        expect(player:GetState("Arms")):ToBe("None")
    end)

    it("reads a parameter from an attribute of what it is parented to, until a script says otherwise", function()
        player.Graph = Graph
        mesh:SetAttribute("Mood", 7)
        step(3)
        expect(player:GetParameter("Mood")):ToBe(7)
        -- A script's value wins on this machine...
        player:SetParameter("Mood", 1)
        step(2)
        expect(player:GetParameter("Mood")):ToBe(1)
        mesh:SetAttribute("Mood", 9)
        step(2)
        expect(player:GetParameter("Mood")):ToBe(1)
        -- ...until it hands the parameter back.
        player:ClearParameter("Mood")
        step(2)
        expect(player:GetParameter("Mood")):ToBe(9)
    end)

    it("refuses a parameter the graph does not declare, by its name", function()
        player.Graph = Graph
        step(3)
        expect(function()
            player:SetParameter("Sped", 1)
        end):ToThrow("Sped")
        expect(function()
            player:ClearParameter("Sped")
        end):ToThrow("Sped")
        expect(function()
            player:SetParameter("Speed", 0 / 0)
        end):ToThrow()
        expect(function()
            (player :: any):SetParameter("Speed", "fast")
        end):ToThrow()
    end)

    it("goes back to tracks alone when its graph is taken away", function()
        player.Graph = Graph
        step(3)
        expect(player:GetState()):ToBe("Idle")
        player.Graph = ""
        step(2)
        expect(player:GetState()):ToBe("")
        expect(player:GetParameter("Speed")):ToBe(nil)
        -- And a track still loads and answers.
        local track = player:LoadAnimation("Walk")
        expect(typeof(track)):ToBe("AnimationTrack")
    end)
end)
