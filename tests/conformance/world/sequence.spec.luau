--!strict
--- `SequencePlayer` (ADR 0204) as a script meets it: a sequence played, held,
--- sought, stopped and skipped, what it says while it does, and what each of
--- those leaves the world as.
---
--- Over the suite's own content: `content/sequences/spec.sequence.json` keys a
--- lamp from a tenth of a second to three tenths, says the line
--- `spec.written` -- a quarter of a second, with no recording -- at three
--- tenths, and has three events. So it is 0.55 s long: the line's slot is
--- part of it.
---
--- What a clip is asked, what a replica does, the camera as a view and the
--- cost are asserted in C++ (`engine_app_tests`, "ADR 0204"); a replay's
--- agreement is the determinism scenario `cutscene`.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local PhysicsService = game:GetService("PhysicsService")

local File = "asset://sequences/spec.sequence.json" :: Content

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
end

type Stage = {
    Player: SequencePlayer,
    Light: PointLight,
    Guide: Part,
    Heard: { string },
    Root: Folder,
}

-- A player of the suite's sequence with its two names bound, and everything
-- it says written down.
local function stage(): Stage
    local root = Instance.new("Folder")
    root.Parent = workspace

    local lamp = Instance.new("Part")
    lamp.Name = "Lamp"
    lamp.Anchored = true
    lamp.Parent = root
    local light = Instance.new("PointLight")
    light.Name = "Light"
    light.Brightness = 2
    light.Parent = lamp

    local guide = Instance.new("Part")
    guide.Name = "Guide"
    guide.Anchored = true
    guide.Parent = root

    local player = Instance.new("SequencePlayer")
    player.Sequence = File
    for name, target in { Lamp = lamp, Guide = guide } :: { [string]: Instance } do
        local binding = Instance.new("SequenceBinding")
        binding.Name = name
        binding.Target = target
        binding.Parent = player
    end
    player.Parent = root

    local heard: { string } = {}
    player.EventReached:Connect(function(name: string, data: any, skipped: boolean)
        local said = name
        if type(data) == "table" then
            said ..= `[{table.concat(data, ",")}]`
        elseif data ~= nil then
            said ..= `[{data}]`
        end
        table.insert(heard, if skipped then `{said} skipped` else said)
    end)
    player.Finished:Connect(function(skipped: boolean)
        table.insert(heard, if skipped then "Finished skipped" else "Finished")
    end)
    return { Player = player, Light = light, Guide = guide, Heard = heard, Root = root }
end

describe("SequencePlayer", function()
    it("is made stopped, at nought, taking the camera and keeping the controls", function()
        local player = Instance.new("SequencePlayer")
        expect(player.Playing):ToBe(false)
        expect(player.TimePosition):ToBe(0)
        expect(player.Speed):ToBe(1)
        expect(player.Looped):ToBe(false)
        expect(player.TakesCamera):ToBe(true)
        expect(player.Controls):ToBe(Enum.SequenceControl.Keep)
        expect(player.Length):ToBe(0)
        player:Destroy()
    end)

    it("refuses a speed of nought and a time before the start", function()
        local player = Instance.new("SequencePlayer")
        expect(function()
            player.Speed = 0
        end):ToThrow()
        expect(function()
            player.TimePosition = -1
        end):ToThrow()
        player.Speed = 2
        player.Controls = Enum.SequenceControl.Suspend
        expect(player.Speed):ToBe(2)
        expect(player.Controls):ToBe(Enum.SequenceControl.Suspend)
        player:Destroy()
    end)

    it("is as long as its file the moment its Sequence is set, and after Play", function()
        -- No tick between: what a script lays a bar out by, or waits for the
        -- end by, is the file's from the line that names it.
        local made = stage()
        expect(made.Player.Length):ToBeCloseTo(0.55, 0.001)
        made.Player:Play()
        expect(made.Player.Length):ToBeCloseTo(0.55, 0.001)
        -- And nought again for a file that is not named.
        made.Player:Stop()
        made.Player.Sequence = "" :: Content
        expect(made.Player.Length):ToBe(0)
        made.Root:Destroy()
    end)

    it("is as long as its last key or its last line's slot, read by the tick", function()
        local made = stage()
        step(2)
        -- The line begins at 0.3 s and takes a quarter of a second to read.
        expect(made.Player.Length):ToBeCloseTo(0.55, 0.001)
        made.Root:Destroy()
    end)

    it(
        "keys a service and what stands under one: the light of the day, the air, the sky, the picture's grade",
        function()
            -- A binding's target is an instance, and a service is one: the day's
            -- hour and the ambient light on `Lighting` itself, the air and the sky
            -- by a path under it, a grade bound by itself.
            local Lighting = game:GetService("Lighting")
            local hour, ambient = Lighting.ClockTime, Lighting.Ambient
            local air = Instance.new("Atmosphere")
            air.Name = "SpecAir"
            air.Parent = Lighting
            local sky = Instance.new("Sky")
            sky.Name = "SpecSky"
            sky.Parent = Lighting
            local focus = Instance.new("DepthOfFieldEffect")
            focus.Name = "SpecFocus"
            focus.Parent = Lighting
            local grade = Instance.new("ColorCorrectionEffect")
            grade.Parent = Lighting

            local player = Instance.new("SequencePlayer")
            player.Sequence = "asset://sequences/services.sequence.json" :: Content
            player.TakesCamera = false
            for name, target in { Lighting = Lighting, Grade = grade } :: { [string]: Instance } do
                local binding = Instance.new("SequenceBinding")
                binding.Name = name
                binding.Target = target
                binding.Parent = player
            end
            player.Parent = workspace
            expect(player.Length):ToBeCloseTo(0.2, 0.001)

            player:Play()
            step(7)
            -- Half way, give or take a tick.
            expect(Lighting.ClockTime > 13 and Lighting.ClockTime < 17):ToBe(true)
            expect(air.Density > 0.05 and air.Density < 0.45):ToBe(true)
            expect(grade.Saturation < -0.1 and grade.Saturation > -0.9):ToBe(true)
            step(12)
            expect(player.Playing):ToBe(false)
            expect(Lighting.ClockTime):ToBeCloseTo(18, 0.001)
            expect(Lighting.Ambient.R):ToBeCloseTo(0.5, 0.001)
            expect(Lighting.Ambient.G):ToBeCloseTo(0.25, 0.001)
            expect(Lighting.Ambient.B):ToBeCloseTo(1, 0.001)
            expect(air.Density):ToBeCloseTo(0.5, 0.001)
            expect(sky.SunAngularSize):ToBeCloseTo(9, 0.001)
            expect(grade.Saturation):ToBeCloseTo(-1, 0.001)
            expect(grade.TintColor.G):ToBeCloseTo(0.5, 0.001)
            expect(grade.TintColor.B):ToBeCloseTo(0, 0.001)
            expect(focus.FocusDistance):ToBeCloseTo(40, 0.001)

            player:Destroy()
            air:Destroy()
            sky:Destroy()
            focus:Destroy()
            grade:Destroy()
            Lighting.ClockTime = hour
            Lighting.Ambient = ambient
        end
    )

    it("stops at a hold of its file until it is released, and says where", function()
        local made = stage()
        made.Player.Sequence = "asset://sequences/shots.sequence.json" :: Content
        made.Player.TakesCamera = false
        local holds: { string } = {}
        made.Player.HoldReached:Connect(function(name: string)
            table.insert(holds, name)
        end)
        expect(made.Player.Held):ToBe("")
        made.Player:Play()
        step(30)
        -- Three tenths in, and no further, however long.
        expect(made.Player.Held):ToBe("Answer")
        expect(made.Player.Playing):ToBe(false)
        expect(made.Player.TimePosition):ToBeCloseTo(0.3, 0.001)
        expect(made.Light.Brightness):ToBeCloseTo(3, 0.05)
        expect(#holds):ToBe(1)
        expect(holds[1]):ToBe("Answer")
        -- Let go, it goes on to its end; the hold is not met again.
        made.Player:Release()
        expect(made.Player.Held):ToBe("")
        step(30)
        expect(made.Player.Playing):ToBe(false)
        expect(made.Player.TimePosition):ToBeCloseTo(0.6, 0.001)
        expect(made.Light.Brightness):ToBeCloseTo(6, 0.001)
        expect(#holds):ToBe(1)
        -- Where it is not held, a release is nothing.
        made.Player:Release()
        expect(made.Player.Playing):ToBe(false)
        made.Root:Destroy()
    end)

    it("has markers: played from one by name, and skipped to the next", function()
        local made = stage()
        made.Player.Sequence = "asset://sequences/shots.sequence.json" :: Content
        made.Player.TakesCamera = false
        local markers = made.Player:GetMarkers()
        expect(#markers):ToBe(3)
        expect(markers[1].Name):ToBe("Opening")
        expect(markers[2].Name):ToBe("Choice")
        expect(markers[2].Time):ToBeCloseTo(0.2, 0.001)
        expect(markers[3].Name):ToBe("Leaving")

        local reached: { string } = {}
        made.Player.MarkerReached:Connect(function(name: string)
            table.insert(reached, name)
        end)
        -- A name the file has not got: nothing is done.
        expect(made.Player:PlayFrom("Nowhere")):ToBe(false)
        expect(made.Player.Playing):ToBe(false)

        expect(made.Player:PlayFrom("Choice")):ToBe(true)
        expect(made.Player.Playing):ToBe(true)
        expect(made.Player.TimePosition):ToBeCloseTo(0.2, 0.001)
        step(2)
        -- Put there as a seek puts it: the lamp where its keys have it.
        expect(made.Light.Brightness > 1.9 and made.Light.Brightness < 2.6):ToBe(true)

        -- To the next shot: past the hold between, the lamp where it would be.
        expect(made.Player:SkipToNext()):ToBe(true)
        step(2)
        expect(made.Player.Held):ToBe("")
        expect(made.Player.Playing):ToBe(true)
        expect(made.Player.TimePosition > 0.39 and made.Player.TimePosition < 0.47):ToBe(true)
        expect(made.Light.Brightness > 3.9):ToBe(true)
        -- And after the last there is no next.
        expect(made.Player:SkipToNext()):ToBe(false)
        step(20)
        expect(made.Player.Playing):ToBe(false)
        expect(table.concat(reached, ",")):ToBe("Choice,Leaving")
        -- A player that names no file has none.
        local empty = Instance.new("SequencePlayer")
        expect(#empty:GetMarkers()):ToBe(0)
        expect(empty:SkipToNext()):ToBe(false)
        empty:Destroy()
        made.Root:Destroy()
    end)

    it("plays to its end: each event once, in order, then Finished, the world where its keys left it", function()
        local made = stage()
        local camera = workspace.CurrentCamera
        made.Player:Play()
        expect(made.Player.Playing):ToBe(true)
        step(4)
        -- Before the lamp's first key, the lamp is the game's.
        expect(made.Light.Brightness):ToBe(2)
        step(8)
        -- Two tenths in: half way between its keys.
        expect(made.Light.Brightness > 0.8 and made.Light.Brightness < 2.2):ToBe(true)
        step(30)
        expect(made.Player.Playing):ToBe(false)
        expect(made.Player.TimePosition):ToBeCloseTo(0.55, 0.001)
        expect(made.Light.Brightness):ToBe(3)
        expect(table.concat(made.Heard, " | ")):ToBe("Begin | Half[1,2,3] | End[warm] | Finished")
        -- The view was the sequence's while it played, and the game's camera
        -- was never written.
        expect(workspace.CurrentCamera):ToBe(camera)
        made.Root:Destroy()
    end)

    it("says its lines from what they are bound to, as DialogueService does", function()
        local made = stage()
        made.Player:Play()
        step(22)
        local voice = made.Guide:FindFirstChildOfClass("Sound") :: Sound?
        expect(voice).Never:ToBe(nil)
        if voice then
            expect(voice.Playing):ToBe(true)
            expect(voice.Category):ToBe(Enum.SoundCategory.Voice)
            expect(voice:FindFirstChildOfClass("Caption")).Never:ToBe(nil)
        end
        made.Player:Stop()
        step(2)
        -- Stopped, what it began is ended.
        if voice and voice.Parent then
            expect(voice.Playing):ToBe(false)
        end
        made.Root:Destroy()
    end)

    it("held, stays where it is; played again, carries on", function()
        local made = stage()
        made.Player:Play()
        step(9)
        made.Player:Pause()
        step(1)
        local held = made.Player.TimePosition
        expect(made.Player.Playing):ToBe(false)
        step(10)
        expect(made.Player.TimePosition):ToBe(held)
        expect(table.concat(made.Heard, " | ")):ToBe("Begin")
        made.Player:Play()
        step(40)
        expect(table.concat(made.Heard, " | ")):ToBe("Begin | Half[1,2,3] | End[warm] | Finished")
        made.Root:Destroy()
    end)

    it("sought, puts every track where it is at that moment and fires nothing", function()
        local made = stage()
        made.Player.TimePosition = 0.2
        step(2)
        expect(made.Light.Brightness):ToBeCloseTo(1.5, 0.001)
        expect(made.Player.TimePosition):ToBeCloseTo(0.2, 0.00001)
        expect(#made.Heard):ToBe(0)
        -- Past the end is the end.
        made.Player.TimePosition = 60
        step(2)
        expect(made.Player.TimePosition):ToBeCloseTo(0.55, 0.001)
        expect(made.Light.Brightness):ToBe(3)
        expect(#made.Heard):ToBe(0)
        made.Root:Destroy()
    end)

    it("stopped, leaves the world as it is, goes back to nought, and finishes nothing", function()
        local made = stage()
        made.Player:Play()
        step(13)
        local reached = made.Light.Brightness
        made.Player:Stop()
        step(3)
        expect(made.Player.Playing):ToBe(false)
        expect(made.Player.TimePosition):ToBe(0)
        expect(made.Light.Brightness):ToBe(reached)
        expect(table.concat(made.Heard, " | ")):ToBe("Begin | Half[1,2,3]")
        made.Root:Destroy()
    end)

    it("skipped, lands the world at the end and says what it passed, marked", function()
        local made = stage()
        made.Player:Play()
        step(3)
        made.Player:Skip()
        step(2)
        expect(made.Player.Playing):ToBe(false)
        expect(made.Player.TimePosition):ToBeCloseTo(0.55, 0.001)
        expect(made.Light.Brightness):ToBe(3)
        expect(table.concat(made.Heard, " | ")):ToBe(
            "Begin | Half[1,2,3] skipped | End[warm] skipped | Finished skipped"
        )
        -- Nothing is said by a skip.
        expect(made.Guide:FindFirstChildOfClass("Sound")):ToBe(nil)
        made.Root:Destroy()
    end)

    it("looped, goes round and never finishes; a name with nothing bound plays the rest", function()
        local made = stage()
        made.Player.Looped = true
        -- Unbound: the lamp's track has nothing to play on, and says so once.
        local lampBinding = made.Player:FindFirstChild("Lamp") :: SequenceBinding
        lampBinding.Target = nil
        made.Player:Play()
        step(50)
        expect(made.Player.Playing):ToBe(true)
        expect(made.Light.Brightness):ToBe(2)
        expect(made.Heard[1]):ToBe("Begin")
        expect(made.Heard[4]):ToBe("Begin")
        expect(table.find(made.Heard, "Finished")):ToBe(nil)
        made.Player:Stop()
        made.Root:Destroy()
    end)

    it("plays a sequence written as a table, a value of it read from an attribute when it is used", function()
        local made = stage()
        made.Player:SetAttribute("Glow", 7)
        made.Player:LoadSequence({
            format = "sequence",
            version = 1,
            bindings = { "Lamp" },
            tracks = {
                {
                    type = "property",
                    target = "Lamp/Light",
                    property = "Brightness",
                    keys = { { time = 0, value = 1 }, { time = 0.2, value = "$Glow" } },
                },
                { type = "event", keys = { { time = 0.1, name = "Written" } } },
            },
        })
        -- A name of its own, which is no file's; and as long as its last key.
        expect(string.sub(made.Player.Sequence :: string, 1, 9)):ToBe("memory://")
        expect(made.Player.Length):ToBeCloseTo(0.2, 0.001)
        made.Player:Play()
        step(7)
        -- Half way from one to what the game said.
        expect(made.Light.Brightness):ToBeCloseTo(4, 0.5)
        -- Said again while it plays, it is what the track goes to from here.
        made.Player:SetAttribute("Glow", 3)
        step(10)
        expect(made.Light.Brightness):ToBe(3)
        expect(table.concat(made.Heard, " | ")):ToBe("Written | Finished")
        made.Root:Destroy()
    end)

    it("says where a table is not a sequence, and is left playing what it played", function()
        local made = stage()
        local ok, why = pcall(function()
            made.Player:LoadSequence({
                format = "sequence",
                version = 1,
                tracks = { { type = "property", target = "Nobody", property = "Brightness", keys = {} } },
            })
        end)
        expect(ok):ToBe(false)
        expect(string.find(tostring(why), "tracks[0].target", 1, true) ~= nil):ToBe(true)
        expect(made.Player.Sequence):ToBe(File)
        made.Root:Destroy()
    end)

    it("brings somebody made of a stamp for as long as a cast track has them there", function()
        local made = stage()
        made.Player:LoadSequence({
            format = "sequence",
            version = 1,
            bindings = {},
            tracks = {
                {
                    type = "cast",
                    as = "Gate",
                    model = "spec-gate",
                    keys = { { time = 0.1 }, { time = 0.3, show = false }, { time = 0.4 } },
                },
                { type = "event", keys = { { time = 0.5, name = "End" } } },
            },
        })
        made.Player:Play()
        step(3)
        expect(made.Player:FindFirstChild("Gate")):ToBe(nil)
        step(6)
        -- There: in the world, and bound under the name the track gave.
        local binding = made.Player:FindFirstChild("Gate") :: SequenceBinding
        expect(binding ~= nil):ToBe(true)
        local gate = binding.Target :: Instance
        expect(gate.ClassName):ToBe("Model")
        expect(gate.Parent):ToBe(workspace)
        expect(gate:FindFirstChild("Rail") ~= nil):ToBe(true)
        -- Gone by a key: out of the world, and the same one when it is back.
        step(12)
        expect(gate.Parent):ToBe(nil)
        step(6)
        expect(gate.Parent):ToBe(workspace)
        -- Stopped, whoever it brought goes with it.
        made.Player:Stop()
        step(2)
        expect(made.Player:FindFirstChild("Gate")):ToBe(nil)
        expect(gate.Parent):ToBe(nil)
        made.Root:Destroy()
    end)

    it("takes a function for its camera, one a player, and lets go of it", function()
        local made = stage()
        local asked = 0
        made.Player:BindCamera(function(cframe: CFrame, _fieldOfView: number, _time: number)
            asked += 1
            return cframe, 40
        end)
        -- Bound again, the one before is let go of; unbound, there is none,
        -- and unbinding where there is none is nothing.
        made.Player:BindCamera(function()
            return nil
        end)
        made.Player:UnbindCamera()
        made.Player:UnbindCamera()
        -- Nothing asks it without a frame that draws the sequence's view.
        expect(asked):ToBe(0)
        expect(pcall(function()
            (made.Player :: any):BindCamera("not a function")
        end)):ToBe(false)
        made.Root:Destroy()
    end)
end)
