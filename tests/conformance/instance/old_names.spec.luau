--!strict
--- Conformance: a member reached by a name it used to have.
---
--- A name changes and the games written against it do not: the old name is the
--- same member, a script that uses it is told once what it is called now, and
--- this file -- which is strict -- still checks, because the type definitions
--- keep the old name too.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local DebugService = game:GetService("DebugService")

--- The warnings said while `run` runs that name `needle`.
local function warningsNaming(needle: string, run: () -> ()): number
    local said = 0
    local capture = DebugService.MessageOut:Connect(function(message: string)
        if string.find(message, needle, 1, true) ~= nil then
            said += 1
        end
    end)
    run()
    task.wait()
    task.wait()
    capture:Disconnect()
    return said
end

describe("a property's old name", function()
    it("reads and writes the property it is now", function()
        local swarm = Instance.new("Swarm")
        swarm.MaxNeighbours = 9
        expect(swarm.MaxNeighbors):ToBe(9)
        swarm.MaxNeighbors = 5
        expect(swarm.MaxNeighbours):ToBe(5)
        swarm:Destroy()
    end)

    it("is one property to a changed signal, whichever name asked for it", function()
        local swarm = Instance.new("Swarm")
        swarm.Parent = workspace
        local heard = 0
        local connection = swarm:GetPropertyChangedSignal("MaxNeighbours"):Connect(function()
            heard += 1
        end)
        swarm.MaxNeighbors = 3
        task.wait()
        expect(heard):ToBe(1)
        connection:Disconnect()
        swarm:Destroy()
    end)

    it("says what it is called now, once, however often it is used", function()
        local gui = Instance.new("SurfaceGui")
        local said = warningsNaming("PixelsPerMeter", function()
            for _ = 1, 20 do
                gui.PixelsPerMetre = gui.PixelsPerMetre + 1
            end
        end)
        -- Forty uses, one warning. Nothing else in the suite uses this old name,
        -- so the one is this spec's.
        expect(said):ToBe(1)
        expect(gui.PixelsPerMeter):ToBe(70)
        gui:Destroy()
    end)
end)

describe("an enum item's old name", function()
    it("is the same item, and the item is called what it is called now", function()
        expect(Enum.PlaybackState.Cancelled):ToBe(Enum.PlaybackState.Canceled)
        expect(Enum.PromiseState.Cancelled):ToBe(Enum.PromiseState.Canceled)
        expect(Enum.SceneLoadStatus.Cancelled):ToBe(Enum.SceneLoadStatus.Canceled)
        expect(Enum.FocusLossReason.Cancelled):ToBe(Enum.FocusLossReason.Canceled)
        -- What an old name cannot keep: the item's name as text.
        expect(Enum.PlaybackState.Cancelled.Name):ToBe("Canceled")
    end)

    it("is not listed twice among the enum's items", function()
        local named = 0
        for _, item in Enum.PlaybackState:GetEnumItems() do
            if item.Name == "Canceled" or item.Name == "Cancelled" then
                named += 1
            end
        end
        expect(named):ToBe(1)
    end)
end)
