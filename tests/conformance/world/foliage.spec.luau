--!strict
--- Conformance: `FoliageLayer` and `FoliageMesh` (ADR 0116), from a script.
---
--- **The foliage itself is not observable** -- it is grown and drawn by the
--- engine, and no script can find a blade -- so what is pinned here is what a
--- script CAN see: the defaults, what each property refuses, where the two
--- classes may live, and the materials a layer grows on.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("FoliageLayer", function()
    it("starts as a grass-thick layer that stays off cliffs", function()
        local layer = Instance.new("FoliageLayer")
        expect(layer.Enabled):ToBe(true)
        expect(layer.Density):ToBe(2)
        expect(layer.SlopeMin):ToBe(0)
        expect(layer.SlopeMax):ToBe(35)
        expect(layer.Clumping):ToBe(0)
        expect(layer.MinSpacing):ToBe(0.25)
        expect(layer.DrawDistance):ToBe(120)
        expect(layer.FadeDistance):ToBe(20)
        expect(layer.Seed):ToBe(0)
        expect(#layer:GetMaterials()):ToBe(0)
        layer:Destroy()
    end)

    it("refuses a negative density, a slope past vertical and clumping past one", function()
        local layer = Instance.new("FoliageLayer") :: any
        expect(function()
            layer.Density = -1
        end):ToThrow()
        expect(function()
            layer.SlopeMax = 91
        end):ToThrow()
        expect(function()
            layer.Clumping = 1.5
        end):ToThrow()
        expect(function()
            layer.DrawDistance = 0
        end):ToThrow()
        layer:Destroy()
    end)

    it("grows on the materials it is given, each with its multiplier", function()
        local layer = Instance.new("FoliageLayer")
        layer:SetMaterials({ { Material = 1 }, { Material = 3, Density = 0.25 } })
        local materials = layer:GetMaterials()
        expect(#materials):ToBe(2)
        expect(materials[1].Material):ToBe(1)
        expect(materials[1].Density):ToBe(1)
        expect(materials[2].Material):ToBe(3)
        expect(materials[2].Density):ToBe(0.25)
        -- An empty list is every material again.
        layer:SetMaterials({})
        expect(#layer:GetMaterials()):ToBe(0)
        expect(function()
            layer:SetMaterials({ { Material = 0 } })
        end):ToThrow()
        layer:Destroy()
    end)

    it("lives under a terrain, and its meshes under it", function()
        local terrain = Instance.new("Terrain")
        terrain.Parent = workspace
        local layer = Instance.new("FoliageLayer")
        layer.Parent = terrain
        local mesh = Instance.new("FoliageMesh")
        mesh.Parent = layer
        expect(mesh.Parent):ToBe(layer)
        expect(layer.Parent):ToBe(terrain)
        terrain:Destroy()
    end)
end)

describe("FoliageMesh", function()
    it("starts upright, a little varied in size, and moved by the wind", function()
        local mesh = Instance.new("FoliageMesh")
        expect(mesh.Mesh):ToBe("")
        expect(mesh.Material):ToBeNil()
        expect(mesh.Weight):ToBe(1)
        expect(mesh.ScaleMin):ToBeCloseTo(0.8, 1e-4)
        expect(mesh.ScaleMax):ToBeCloseTo(1.2, 1e-4)
        expect(mesh.RandomRotation):ToBe(true)
        expect(mesh.AlignToNormal):ToBe(0)
        expect(mesh.WindResponse):ToBe(1)
        expect(mesh.Stiffness):ToBe(1)
        expect(mesh.CastShadow):ToBe(true)
        mesh:Destroy()
    end)

    it("refuses a scale of zero and a wind response past one", function()
        local mesh = Instance.new("FoliageMesh") :: any
        expect(function()
            mesh.ScaleMin = 0
        end):ToThrow()
        expect(function()
            mesh.WindResponse = 2
        end):ToThrow()
        expect(function()
            mesh.Stiffness = 0
        end):ToThrow()
        mesh:Destroy()
    end)
end)
