--!strict
--- Conformance: `DragDetector` (ADR 0126 §3), its properties from a script.
---
--- What a drag does with the pointer is the engine's and is held by the C++
--- tests that drive a pointer; what a script sets and reads back -- and what
--- it is refused -- is the surface a game is written against.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local beforeEach, afterEach = testing.beforeEach, testing.afterEach

describe("drag detector", function()
    local part: Part
    local drag: DragDetector

    beforeEach(function()
        part = Instance.new("Part")
        part.Anchored = true
        part.Parent = workspace
        drag = Instance.new("DragDetector")
        drag.Parent = part
    end)

    afterEach(function()
        part:Destroy()
    end)

    it("starts as a plane drag that puts the part where it is dragged", function()
        expect(drag.DragStyle):ToBe(Enum.DragDetectorDragStyle.TranslatePlane)
        expect(drag.ResponseStyle):ToBe(Enum.DragDetectorResponseStyle.Geometric)
        expect(drag.Axis):ToBe(vector.create(0, 1, 0))
        expect(drag.ReferenceInstance):ToBe(nil)
        expect(drag.MaxActivationDistance):ToBe(32)
        expect(drag.MinDragTranslation):ToBe(0)
        expect(drag.MaxDragTranslation):ToBe(0)
        expect(drag.Enabled):ToBe(true)
    end)

    it("keeps the shape, the limits and the reference it is given", function()
        drag.DragStyle = Enum.DragDetectorDragStyle.RotateAxis
        drag.Axis = vector.create(0, 0, 2)
        drag.MinDragAngle = -30
        drag.MaxDragAngle = 60
        drag.ReferenceInstance = part
        drag.ResponseStyle = Enum.DragDetectorResponseStyle.Physical
        drag.MaxForce = 500
        expect(drag.DragStyle):ToBe(Enum.DragDetectorDragStyle.RotateAxis)
        expect(drag.Axis):ToBe(vector.create(0, 0, 2))
        expect(drag.MinDragAngle):ToBe(-30)
        expect(drag.MaxDragAngle):ToBe(60)
        expect(drag.ReferenceInstance):ToBe(part)
        expect(drag.MaxForce):ToBe(500)
        drag.ReferenceInstance = nil
        expect(drag.ReferenceInstance):ToBe(nil)
    end)

    it("refuses an axis that is no direction, and a force or a reach below zero", function()
        expect(function()
            drag.Axis = vector.create(0, 0, 0)
        end):ToThrow()
        expect(function()
            drag.Axis = vector.create(0 / 0, 1, 0)
        end):ToThrow()
        expect(function()
            drag.MaxForce = -1
        end):ToThrow()
        expect(function()
            drag.MaxActivationDistance = -1
        end):ToThrow()
        expect(function()
            drag.MaxDragTranslation = math.huge
        end):ToThrow()
        expect(drag.Axis):ToBe(vector.create(0, 1, 0))
    end)
end)
