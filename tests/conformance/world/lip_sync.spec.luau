--!strict
--- `LipSync` (ADR 0200, section 9): the instance that moves a mouth with what
--- is said. What it is made with, what it takes, and what it refuses -- the
--- moving itself is a picture, and is held where a face is (the app's tests).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("LipSync", function()
    it("is made following the nearest voice, by the best way there is", function()
        local mouth = Instance.new("LipSync")
        expect(mouth.Source):ToBeNil()
        expect(mouth.Mode):ToBe(Enum.LipSyncMode.Auto)
        expect(mouth.Map):ToBe("")
        expect(mouth.Smoothing):ToBeCloseTo(0.06, 5)
        expect(mouth.Weight):ToBe(1)
        expect(mouth.Enabled):ToBe(true)
        mouth:Destroy()
    end)

    it("takes the sound it follows, and lets go of it", function()
        local face = Instance.new("MeshPart")
        local voice = Instance.new("Sound")
        voice.Category = Enum.SoundCategory.Voice
        voice.Parent = face
        local mouth = Instance.new("LipSync")
        mouth.Parent = face
        mouth.Source = voice
        expect(mouth.Source):ToBe(voice)
        mouth.Mode = Enum.LipSyncMode.Loudness
        expect(mouth.Mode):ToBe(Enum.LipSyncMode.Loudness)
        mouth.Map = "asset://models/other.face.json"
        expect(mouth.Map):ToBe("asset://models/other.face.json")
        mouth.Source = nil
        expect(mouth.Source):ToBeNil()
        face:Destroy()
    end)

    it("refuses a weight or an easing outside what a face can show", function()
        local mouth = Instance.new("LipSync")
        mouth.Weight = 2
        expect(mouth.Weight):ToBe(2)
        expect(function()
            mouth.Weight = 2.5
        end):ToThrow()
        expect(function()
            mouth.Weight = -0.1
        end):ToThrow()
        mouth.Smoothing = 0
        expect(mouth.Smoothing):ToBe(0)
        expect(function()
            mouth.Smoothing = 1.5
        end):ToThrow()
        expect(mouth.Weight):ToBe(2)
        mouth:Destroy()
    end)
end)
