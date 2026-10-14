--!strict
--- Conformance: the wind (ADR 0115). Visual only, and the same function a
--- script reads with `GetWindAt` as the renderer draws with.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

describe("wind", function()
    afterEach(function()
        workspace.GlobalWind = vector.create(0, 0, 0)
        workspace.WindGusts = 0
        workspace.WindTurbulence = 0
    end)

    it("is still until a game says otherwise", function()
        expect(workspace.GlobalWind):ToBe(vector.create(0, 0, 0))
        expect(workspace:GetWindAt(vector.create(10, 0, -4))):ToBe(vector.create(0, 0, 0))
    end)

    it("reads back the global wind where it is steady", function()
        workspace.GlobalWind = vector.create(6, 0, 0)
        local wind = workspace:GetWindAt(vector.create(35, 2, 11))
        expect(wind.x):ToBeCloseTo(6, 3)
        expect(wind.z):ToBeCloseTo(0, 3)
    end)

    it("gusts and turns within what the settings allow", function()
        workspace.GlobalWind = vector.create(0, 0, 10)
        workspace.WindGusts = 0.5
        workspace.WindTurbulence = 1
        for x = 0, 200, 13 do
            local speed = vector.magnitude(workspace:GetWindAt(vector.create(x, 0, x * 0.3)))
            expect(speed >= 5 - 0.01 and speed <= 15 + 0.01):ToBe(true)
        end
    end)

    it("refuses settings outside zero to one", function()
        expect(function()
            workspace.WindGusts = 2
        end):ToThrow()
        expect(function()
            workspace.WindTurbulence = -0.1
        end):ToThrow()
    end)

    it("is a setting a particle emitter can follow", function()
        local part = Instance.new("Part")
        part.Anchored = true
        part.Parent = workspace
        local emitter = Instance.new("ParticleEmitter")
        expect(emitter.WindAffectsDrift):ToBe(false)
        emitter.WindAffectsDrift = true
        emitter.Parent = part
        expect(emitter.WindAffectsDrift):ToBe(true)
        part:Destroy()
    end)
end)
