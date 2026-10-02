--!strict
--- Conformance: sound effects and vibration (ADR 0131).
---
--- Nine effects a `Sound` or an `AudioGroup` is played through, and the
--- service that shakes a controller. What either DOES is downstream of
--- anything a script can read -- samples and motors -- and is asserted where
--- those can be: `audio_tests.cpp` renders each effect offline and
--- `platform_tests.cpp` stands a fake device where the hardware is. Here is
--- what a script can see: that each class exists with the numbers its page
--- says, keeps what it is given, and refuses what is out of range; and that
--- a run with no hands answers so and takes every call quietly.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local HapticService = game:GetService("HapticService")

local spawned: { Instance } = {}

local function keep<T>(instance: T & Instance): T
    table.insert(spawned, instance)
    return instance
end

afterEach(function()
    for _, instance in spawned do
        instance:Destroy()
    end
    table.clear(spawned)
end)

describe("Sound effects", function()
    it("each begins with the numbers its page says", function()
        local reverb = keep(Instance.new("ReverbSoundEffect"))
        expect(reverb.Enabled):ToBe(true)
        expect(reverb.Priority):ToBe(0)
        expect(reverb.RoomSize):ToBe(0.5)
        expect(reverb.Damping):ToBe(0.5)
        expect(reverb.WetLevel):ToBeCloseTo(0.3, 1e-6)
        expect(reverb.DryLevel):ToBe(1)
        expect(reverb.Width):ToBe(1)

        local echo = keep(Instance.new("EchoSoundEffect"))
        expect(echo.Delay):ToBeCloseTo(0.3, 1e-6)
        expect(echo.Feedback):ToBeCloseTo(0.4, 1e-6)
        expect(echo.WetLevel):ToBe(0.5)
        expect(echo.DryLevel):ToBe(1)

        local equalizer = keep(Instance.new("EqualizerSoundEffect"))
        expect(equalizer.LowGain):ToBe(0)
        expect(equalizer.MidGain):ToBe(0)
        expect(equalizer.HighGain):ToBe(0)
        expect(equalizer.MidLow):ToBe(400)
        expect(equalizer.MidHigh):ToBe(4000)

        expect(keep(Instance.new("LowPassSoundEffect")).Cutoff):ToBe(2000)
        expect(keep(Instance.new("HighPassSoundEffect")).Cutoff):ToBe(500)
        expect(keep(Instance.new("LowPassSoundEffect")).Resonance):ToBeCloseTo(0.707, 1e-6)
        expect(keep(Instance.new("DistortionSoundEffect")).Level):ToBe(0.5)

        local compressor = keep(Instance.new("CompressorSoundEffect"))
        expect(compressor.Threshold):ToBe(-20)
        expect(compressor.Ratio):ToBe(4)
        expect(compressor.Attack):ToBeCloseTo(0.01, 1e-6)
        expect(compressor.Release):ToBeCloseTo(0.1, 1e-6)
        expect(compressor.MakeupGain):ToBe(0)

        local chorus = keep(Instance.new("ChorusSoundEffect"))
        expect(chorus.Rate):ToBe(0.5)
        expect(chorus.Depth):ToBe(0.5)
        expect(chorus.Mix):ToBe(0.5)

        expect(keep(Instance.new("PitchShiftSoundEffect")).Octave):ToBe(1.25)
    end)

    it("sits under a sound or a group, keeps what it is given, and is a SoundEffect", function()
        local sound = keep(Instance.new("Sound"))
        local group = keep(Instance.new("AudioGroup"))
        local filter = keep(Instance.new("LowPassSoundEffect"))
        filter.Cutoff = 300
        filter.Resonance = 2
        filter.Priority = 5
        filter.Enabled = false
        filter.Parent = sound
        expect(filter.Parent):ToBe(sound)
        expect(filter.Cutoff):ToBe(300)
        expect(filter.Resonance):ToBe(2)
        expect(filter.Priority):ToBe(5)
        expect(filter.Enabled):ToBe(false)
        expect(filter:IsA("SoundEffect")):ToBe(true)

        local room = keep(Instance.new("ReverbSoundEffect"))
        room.RoomSize = 0.9
        room.Parent = group
        expect(room.Parent):ToBe(group)
        expect(room.RoomSize):ToBeCloseTo(0.9, 1e-6)

        -- The class that names them all cannot be made by itself.
        expect(function()
            Instance.new("SoundEffect" :: any)
        end):ToThrow()
    end)

    it("refuses a number outside its range", function()
        expect(function()
            keep(Instance.new("LowPassSoundEffect")).Cutoff = 5
        end):ToThrow()
        expect(function()
            keep(Instance.new("EchoSoundEffect")).Feedback = 1
        end):ToThrow()
        expect(function()
            keep(Instance.new("EchoSoundEffect")).Delay = 3
        end):ToThrow()
        expect(function()
            keep(Instance.new("CompressorSoundEffect")).Ratio = 0.5
        end):ToThrow()
        expect(function()
            keep(Instance.new("PitchShiftSoundEffect")).Octave = 4
        end):ToThrow()
        expect(function()
            keep(Instance.new("EqualizerSoundEffect")).LowGain = 40
        end):ToThrow()
        expect(function()
            keep(Instance.new("DistortionSoundEffect")).Level = 0 / 0
        end):ToThrow()
    end)

    it("leaves a sound's own timeline alone", function()
        -- What the speakers do is downstream of the simulation: a sound
        -- through an effect is as long as one without, and is as far along.
        local AudioService = game:GetService("AudioService")
        local plain = keep(Instance.new("Sound"))
        plain.Parent = AudioService
        local shifted = keep(Instance.new("Sound"))
        shifted.Parent = AudioService
        local shift = keep(Instance.new("PitchShiftSoundEffect"))
        shift.Octave = 2
        shift.Parent = shifted
        local echo = keep(Instance.new("EchoSoundEffect"))
        echo.Parent = shifted
        plain:Play()
        shifted:Play()
        task.wait(0.1)
        expect(shifted.Playing):ToBe(true)
        expect(shifted.TimeLength):ToBe(plain.TimeLength)
        expect(shifted.TimePosition):ToBe(plain.TimePosition)
    end)
end)

describe("HapticService", function()
    it("answers for whatever is there, and takes every call quietly", function()
        -- A gamepad may be plugged into the machine this runs on: what is
        -- asked is an answer, not which.
        expect(type(HapticService:IsVibrationSupported(Enum.UserInputType.Gamepad))):ToBe("boolean")
        expect(type(HapticService:IsVibrationSupported(Enum.UserInputType.Touch))):ToBe("boolean")
        expect(type(HapticService:IsMotorSupported(Enum.UserInputType.Gamepad, Enum.VibrationMotor.Large))):ToBe(
            "boolean"
        )
        -- A keyboard never does, and no device has a motor for each hand.
        expect(HapticService:IsVibrationSupported(Enum.UserInputType.Keyboard)):ToBe(false)
        expect(HapticService:IsMotorSupported(Enum.UserInputType.Gamepad, Enum.VibrationMotor.LeftHand)):ToBe(false)
        expect(HapticService:IsMotorSupported(Enum.UserInputType.Touch, Enum.VibrationMotor.Large)):ToBe(false)
        HapticService:SetMotor(Enum.UserInputType.Gamepad, Enum.VibrationMotor.Large, 1)
        HapticService:SetMotor(Enum.UserInputType.Gamepad, Enum.VibrationMotor.Large, 0)
        HapticService:SetMotor(Enum.UserInputType.Keyboard, Enum.VibrationMotor.Small, 0.5)
        HapticService:Vibrate(0.5, 0.1)
    end)

    it("refuses a level outside nought to one and a length it could not mean", function()
        expect(function()
            HapticService:SetMotor(Enum.UserInputType.Gamepad, Enum.VibrationMotor.Large, 2)
        end):ToThrow()
        expect(function()
            HapticService:SetMotor(Enum.UserInputType.Gamepad, Enum.VibrationMotor.Large, -1)
        end):ToThrow()
        expect(function()
            HapticService:Vibrate(1.5, 1)
        end):ToThrow()
        expect(function()
            HapticService:Vibrate(1, 60)
        end):ToThrow()
        expect(function()
            (HapticService :: any):SetMotor(Enum.KeyCode.A, Enum.VibrationMotor.Large, 1)
        end):ToThrow()
    end)
end)
