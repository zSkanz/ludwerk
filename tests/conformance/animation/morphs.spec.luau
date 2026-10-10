--!strict
--- A `MeshPart`'s morph targets from a script (ADR 0196).
---
--- **These cases run against a mesh that is not there**, as the ones beside
--- them do: the conformance suite is headless and loads no glTF, so the part
--- has no targets to name. That is the state a real game is in for the frames
--- between making a `MeshPart` and its file being read -- and a script that
--- sets a blink in that moment must be kept, not refused. What a weight does
--- to a loaded mesh, and what a clip does to one, is asserted in C++
--- (`engine_render_tests`: `morph_weights_tests.cpp`) and in the picture the
--- `morph_landing` gate compares.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local beforeEach, afterEach = testing.beforeEach, testing.afterEach

describe("MeshPart morph targets", function()
    local mesh: MeshPart = nil :: any

    beforeEach(function()
        mesh = Instance.new("MeshPart")
        mesh.Name = "MorphSpecMesh"
        mesh.Anchored = true
        mesh.Parent = workspace
    end)

    afterEach(function()
        mesh:Destroy()
    end)

    it("names no targets before its file has loaded", function()
        local names = mesh:GetMorphTargets()
        expect(#names):ToBe(0)
        -- And a weight nobody set, of a target nobody has, is nought.
        expect(mesh:GetMorphWeight("Blink")):ToBe(0)
    end)

    it("keeps a weight set before the mesh arrives, by its name", function()
        mesh:SetMorphWeight("Blink", 1)
        expect(mesh:GetMorphWeight("Blink")):ToBe(1)
        -- Set again, it is replaced.
        mesh:SetMorphWeight("Blink", 0.25)
        expect(mesh:GetMorphWeight("Blink")):ToBe(0.25)
        -- And another target's is its own.
        expect(mesh:GetMorphWeight("Smile")):ToBe(0)
    end)

    it("takes a weight past one and below nought", function()
        mesh:SetMorphWeight("Jaw", 1.5)
        mesh:SetMorphWeight("Brow", -0.5)
        expect(mesh:GetMorphWeight("Jaw")):ToBe(1.5)
        expect(mesh:GetMorphWeight("Brow")):ToBe(-0.5)
    end)

    it("gives a target back when its weight is cleared", function()
        mesh:SetMorphWeight("Blink", 1)
        mesh:ClearMorphWeight("Blink")
        expect(mesh:GetMorphWeight("Blink")):ToBe(0)
        -- Clearing what was never set is not an error.
        mesh:ClearMorphWeight("Blink")
        mesh:ClearMorphWeight("NeverSet")
    end)

    it("refuses a weight that is not a number at all", function()
        expect(function()
            mesh:SetMorphWeight("Blink", 0 / 0)
        end):ToThrow("script.err.morph_weight_not_finite")
        expect(function()
            mesh:SetMorphWeight("Blink", math.huge)
        end):ToThrow()
        expect(function()
            (mesh :: any):SetMorphWeight("Blink", "wide")
        end):ToThrow()
        -- And what it refused it did not keep.
        expect(mesh:GetMorphWeight("Blink")):ToBe(0)
    end)

    it("does not hand one part's weight to the next", function()
        mesh:SetMorphWeight("Blink", 1)
        local other = Instance.new("MeshPart")
        other.Anchored = true
        other.Parent = workspace
        expect(other:GetMorphWeight("Blink")):ToBe(0)
        other:Destroy()
    end)
end)
