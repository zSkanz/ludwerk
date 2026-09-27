--!strict
--- `ColorSequence`, `NumberSequence` and their keypoints, from api-design.md
--- §2.3 and ADR 0110: two to twenty stops, the first at 0 and the last at 1,
--- times never falling, immutable, compared by value.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("ColorSequenceKeypoint", function()
    it("carries a time and a colour, and is typeof ColorSequenceKeypoint", function()
        local stop = ColorSequenceKeypoint.new(0.25, Color3.new(1, 0, 0))
        expect(typeof(stop)):ToBe("ColorSequenceKeypoint")
        expect(stop.Time):ToBe(0.25)
        expect(stop.Value):ToBe(Color3.new(1, 0, 0))
    end)

    it("compares by value", function()
        expect(ColorSequenceKeypoint.new(0.5, Color3.new(0, 1, 0))):ToBe(
            ColorSequenceKeypoint.new(0.5, Color3.new(0, 1, 0))
        )
    end)

    it("refuses a time outside 0 to 1", function()
        expect(function()
            ColorSequenceKeypoint.new(1.5, Color3.new())
        end):ToThrow()
        expect(function()
            ColorSequenceKeypoint.new(-0.1, Color3.new())
        end):ToThrow()
    end)
end)

describe("NumberSequenceKeypoint", function()
    it("carries a time, a value and an envelope that defaults to zero", function()
        local stop = NumberSequenceKeypoint.new(0.5, 3)
        expect(typeof(stop)):ToBe("NumberSequenceKeypoint")
        expect(stop.Time):ToBe(0.5)
        expect(stop.Value):ToBe(3)
        expect(stop.Envelope):ToBe(0)
        expect(NumberSequenceKeypoint.new(0.5, 3, 0.25).Envelope):ToBe(0.25)
    end)
end)

describe("ColorSequence", function()
    it("is one colour from start to end when given one", function()
        local sequence = ColorSequence.new(Color3.new(0, 0, 1))
        expect(typeof(sequence)):ToBe("ColorSequence")
        local stops = sequence.Keypoints
        expect(#stops):ToBe(2)
        expect(stops[1].Time):ToBe(0)
        expect(stops[2].Time):ToBe(1)
        expect(stops[1].Value):ToBe(Color3.new(0, 0, 1))
        expect(stops[2].Value):ToBe(Color3.new(0, 0, 1))
    end)

    it("blends from one colour to another when given two", function()
        local stops = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1)).Keypoints
        expect(stops[1].Value):ToBe(Color3.new(1, 0, 0))
        expect(stops[2].Value):ToBe(Color3.new(0, 0, 1))
    end)

    it("keeps the stops it is given, in order", function()
        local sequence = ColorSequence.new({
            ColorSequenceKeypoint.new(0, Color3.new(1, 0, 0)),
            ColorSequenceKeypoint.new(0.5, Color3.new(0, 1, 0)),
            ColorSequenceKeypoint.new(1, Color3.new(0, 0, 1)),
        })
        local stops = sequence.Keypoints
        expect(#stops):ToBe(3)
        expect(stops[2].Time):ToBe(0.5)
        expect(stops[2].Value):ToBe(Color3.new(0, 1, 0))
    end)

    it("allows two stops at one time, a hard edge", function()
        local sequence = ColorSequence.new({
            ColorSequenceKeypoint.new(0, Color3.new(1, 0, 0)),
            ColorSequenceKeypoint.new(0.5, Color3.new(1, 0, 0)),
            ColorSequenceKeypoint.new(0.5, Color3.new(0, 0, 1)),
            ColorSequenceKeypoint.new(1, Color3.new(0, 0, 1)),
        })
        expect(#sequence.Keypoints):ToBe(4)
    end)

    it("refuses stops that do not start at 0, end at 1 and rise", function()
        expect(function()
            ColorSequence.new({
                ColorSequenceKeypoint.new(0.1, Color3.new()),
                ColorSequenceKeypoint.new(1, Color3.new()),
            })
        end):ToThrow()
        expect(function()
            ColorSequence.new({
                ColorSequenceKeypoint.new(0, Color3.new()),
                ColorSequenceKeypoint.new(0.9, Color3.new()),
            })
        end):ToThrow()
        expect(function()
            ColorSequence.new({
                ColorSequenceKeypoint.new(0, Color3.new()),
                ColorSequenceKeypoint.new(0.7, Color3.new()),
                ColorSequenceKeypoint.new(0.3, Color3.new()),
                ColorSequenceKeypoint.new(1, Color3.new()),
            })
        end):ToThrow()
        expect(function()
            ColorSequence.new({ ColorSequenceKeypoint.new(0, Color3.new()) })
        end):ToThrow()
    end)

    it("refuses more than twenty stops", function()
        local stops = {}
        for index = 0, 20 do
            table.insert(stops, ColorSequenceKeypoint.new(index / 20, Color3.new()))
        end
        expect(function()
            ColorSequence.new(stops)
        end):ToThrow()
        table.remove(stops, 11)
        expect(#ColorSequence.new(stops).Keypoints):ToBe(20)
    end)

    it("hands back a new table of stops each read, so changing it changes nothing", function()
        local sequence = ColorSequence.new(Color3.new(1, 1, 1))
        local first = sequence.Keypoints
        table.remove(first, 1)
        expect(#sequence.Keypoints):ToBe(2)
    end)

    it("compares every stop", function()
        expect(ColorSequence.new(Color3.new(1, 0, 0))):ToBe(ColorSequence.new(Color3.new(1, 0, 0)))
        expect(ColorSequence.new(Color3.new(1, 0, 0))).Never:ToBe(ColorSequence.new(Color3.new(1, 0, 0.5)))
    end)

    it("is immutable", function()
        expect(function()
            (ColorSequence.new(Color3.new()) :: any).Keypoints = {}
        end):ToThrow()
    end)
end)

describe("NumberSequence", function()
    it("is one number, or two blended, or the stops given", function()
        local one = NumberSequence.new(0.5).Keypoints
        expect(one[1].Value):ToBe(0.5)
        expect(one[2].Value):ToBe(0.5)

        local two = NumberSequence.new(0, 1).Keypoints
        expect(two[1].Value):ToBe(0)
        expect(two[2].Value):ToBe(1)

        local many = NumberSequence.new({
            NumberSequenceKeypoint.new(0, 1),
            NumberSequenceKeypoint.new(0.25, 0),
            NumberSequenceKeypoint.new(1, 1, 0.5),
        }).Keypoints
        expect(#many):ToBe(3)
        expect(many[3].Envelope):ToBe(0.5)
        expect(typeof(NumberSequence.new(0))):ToBe("NumberSequence")
    end)

    it("refuses stops that break the rules", function()
        expect(function()
            NumberSequence.new({ NumberSequenceKeypoint.new(0, 0), NumberSequenceKeypoint.new(0.5, 0) })
        end):ToThrow()
    end)

    it("compares every stop", function()
        expect(NumberSequence.new(0, 1)):ToBe(NumberSequence.new(0, 1))
        expect(NumberSequence.new(0, 1)).Never:ToBe(NumberSequence.new(0, 0.5))
    end)
end)
