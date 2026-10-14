--!strict
--- Conformance: SaveService and SaveSlot (ADR 0111).
---
--- A conformance run saves into a folder of its own, so these slots never
--- meet a player's. Every value kind a slot holds comes back as that kind,
--- what a save cannot hold is refused at `Set`, and `Changed` is deferred
--- like every other signal.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local SaveService = game:GetService("SaveService")

describe("SaveService", function()
    it("hands back a slot by name, the same slot every time", function()
        local slot = SaveService:GetSlotAsync("conformance_basic")
        expect(slot.Name):ToBe("conformance_basic")
        expect(slot.Recovered):ToBe(true)
        slot:Set("marker", 1)
        expect(SaveService:GetSlotAsync("conformance_basic"):Get("marker")):ToBe(1)
    end)

    it("refuses a name that is not a slot name", function()
        expect(function()
            SaveService:GetSlotAsync("../outside")
        end):ToThrow()
        expect(function()
            SaveService:GetSlotAsync("")
        end):ToThrow()
    end)

    it("keeps every kind of value as that kind", function()
        local slot = SaveService:GetSlotAsync("conformance_kinds")
        slot:Set("text", "hello")
        slot:Set("number", 0.5)
        slot:Set("flag", true)
        slot:Set("vector", Vector3.new(1, 2, 3))
        slot:Set("colour", Color3.new(0.25, 0.5, 0.75))
        slot:Set("frame", CFrame.new(1, 2, 3))
        slot:Set("size", UDim2.new(0.5, 10, 0.25, 4))
        slot:Set("nested", { list = { 1, 2, 3 }, name = "bag" })
        expect(slot:Get("text")):ToBe("hello")
        expect(slot:Get("number")):ToBe(0.5)
        expect(slot:Get("flag")):ToBe(true)
        expect(typeof(slot:Get("vector"))):ToBe("vector")
        expect(typeof(slot:Get("colour"))):ToBe("Color3")
        expect(typeof(slot:Get("frame"))):ToBe("CFrame")
        expect(typeof(slot:Get("size"))):ToBe("UDim2")
        local nested = slot:Get("nested")
        expect(#nested.list):ToBe(3)
        expect(nested.name):ToBe("bag")
    end)

    it("refuses what a save cannot hold, and keeps what it had", function()
        local slot = SaveService:GetSlotAsync("conformance_refusals")
        slot:Set("kept", 7)
        expect(function()
            slot:Set("kept", function() end)
        end):ToThrow()
        expect(function()
            slot:Set("part", workspace)
        end):ToThrow()
        expect(slot:Get("kept")):ToBe(7)
    end)

    it("fires Changed after a change, not during it", function()
        local slot = SaveService:GetSlotAsync("conformance_changed")
        local seen: { string } = {}
        local connection = slot.Changed:Connect(function(key: string)
            table.insert(seen, key)
        end)
        slot:Set("a", 1)
        expect(#seen):ToBe(0)
        task.wait()
        expect(#seen):ToBe(1)
        expect(seen[1]):ToBe("a")
        connection:Disconnect()
    end)

    it("writes a slot on SaveAsync and lists it", function()
        local slot = SaveService:GetSlotAsync("conformance_written")
        slot:Set("score", 42)
        slot:SaveAsync()
        expect(table.find(SaveService:ListSlots(), "conformance_written") ~= nil):ToBe(true)
        SaveService:DeleteSlot("conformance_written")
        expect(slot:Get("score")):ToBe(nil)
    end)
end)
