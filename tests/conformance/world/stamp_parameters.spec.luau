--!strict
--- Conformance: a stamp's parameters (ADR 0155 §6, §7).
---
--- A stamp declares the handful of values a designer sets -- here a gate's
--- width, its style and whether it is open -- and each is an attribute of a
--- copy's root. `Instance.stamp` sets them before anything else runs on the
--- copy, checked against the declaration and refused, never clamped; writing
--- one later drives what it drives at once. The stamp is
--- `content/stamps/spec-gate.stamp.json`: its width drives its rail's `Size.X`.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local function rail(gate: Instance): Part
    return gate:FindFirstChild("Rail") :: Part
end

describe("a stamp's parameters", function()
    it("start at their defaults, and drive what they drive", function()
        local gate = Instance.stamp("spec-gate")
        expect(gate:GetAttribute("Width")):ToBe(2)
        expect(gate:GetAttribute("Style")):ToBe("plain")
        expect(gate:GetAttribute("Open")):ToBe(false)
        expect(rail(gate).Size.x):ToBeCloseTo(2, 0.0001)
        gate:Destroy()
    end)

    it("are set by Instance.stamp before the copy is handed over", function()
        local gate = Instance.stamp("spec-gate", true, { Width = 6, Style = "fancy" })
        expect(gate:GetAttribute("Width")):ToBe(6)
        expect(gate:GetAttribute("Style")):ToBe("fancy")
        expect(rail(gate).Size.x):ToBeCloseTo(6, 0.0001)
        gate:Destroy()
    end)

    it("drive at once when one is written in play", function()
        local gate = Instance.stamp("spec-gate")
        gate:SetAttribute("Width", 3.5)
        expect(rail(gate).Size.x):ToBeCloseTo(3.5, 0.0001)
        gate:Destroy()
    end)

    it("are refused, never clamped, when a value does not fit the declaration", function()
        expect(function()
            Instance.stamp("spec-gate", true, { Width = 40 })
        end):ToThrow()
        expect(function()
            Instance.stamp("spec-gate", true, { Style = "odd" })
        end):ToThrow()
        expect(function()
            Instance.stamp("spec-gate", true, { Width = "wide" } :: any)
        end):ToThrow()
        expect(function()
            Instance.stamp("spec-gate", true, { Height = 2 })
        end):ToThrow()
    end)
end)

describe("Instance:GetStamp", function()
    it("names the stamp a linked copy came from, and nothing for anything else", function()
        local linked = Instance.stamp("spec-gate")
        expect(linked:GetStamp()):ToBe("stamps/spec-gate.stamp.json")
        expect((rail(linked) :: Instance):GetStamp()).Never:ToBe("stamps/spec-gate.stamp.json")
        local unlinked = Instance.stamp("spec-gate", false)
        expect(unlinked:GetStamp()):ToBeNil()
        expect(Instance.new("Part"):GetStamp()):ToBeNil()
        linked:Destroy()
        unlinked:Destroy()
    end)
end)
