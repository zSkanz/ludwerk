--!strict
--- `@engine/npclod`, `Workspace:GetNearestPlayerDistance` and
--- `BasePart.Dormancy` (ADR 0234).
---
--- The suite runs alone, with no player who has a character: the nearest
--- player is no distance at all, so everything is far, and nothing sleeps --
--- only a dedicated server sleeps anything. What sleeping does to a character
--- is asserted in C++ (`engine_scene_tests`, the dormancy case); what the
--- module calls, and how often, is asserted here.

local npclod = require("@engine/npclod")
local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local PhysicsService = game:GetService("PhysicsService")

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
end

describe("npclod", function()
    it("says no player is near when none has a character", function()
        expect(workspace:GetNearestPlayerDistance(vector.create(0, 0, 0))):ToBe(math.huge)
    end)

    it("calls a far NPC every sixteenth tick, with the time since the last call", function()
        local guard = Instance.new("Part")
        guard.Name = "NpcLodGuard"
        guard.Anchored = true
        guard.Parent = workspace
        local calls = 0
        local lastDt = 0
        local thinker = npclod.think(guard, function(dt: number)
            calls += 1
            lastDt = dt
        end)
        step(64)
        thinker:Stop()
        expect(calls >= 3 and calls <= 5):ToBe(true)
        expect(math.abs(lastDt - 16 * PhysicsService.FixedTimestep) < 0.001):ToBe(true)
        guard:Destroy()
    end)

    it("follows the rates the game sets", function()
        npclod.configure({ FarEvery = 4 })
        local guard = Instance.new("Part")
        guard.Name = "NpcLodQuick"
        guard.Anchored = true
        guard.Parent = workspace
        local calls = 0
        local thinker = npclod.think(guard, function()
            calls += 1
        end)
        step(64)
        thinker:Stop()
        npclod.configure({ FarEvery = 16 })
        expect(calls >= 15 and calls <= 17):ToBe(true)
        guard:Destroy()
    end)

    it("stops when its NPC leaves the world", function()
        local guard = Instance.new("Part")
        guard.Name = "NpcLodGone"
        guard.Anchored = true
        guard.Parent = workspace
        local thinker = npclod.think(guard, function() end)
        guard:Destroy()
        step(2)
        expect(thinker.IsThinking):ToBe(false)
    end)

    it("keeps a part's dormancy, which only a dedicated server acts on", function()
        local part = Instance.new("Part")
        expect(part.Dormancy):ToBe(Enum.Dormancy.Auto)
        expect(part.Dormant):ToBe(false)
        part.Dormancy = Enum.Dormancy.Never
        expect(part.Dormancy):ToBe(Enum.Dormancy.Never)
        part:Destroy()
    end)
end)
