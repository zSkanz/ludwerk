--!strict

-- **Faces that talk, smile and blink** (ADR 0196): three busts of one model
-- whose face has five shape keys -- `Blink`, `Smile`, `JawOpen`, `BrowUp`,
-- `BrowDown`.
--
-- The one on the left TALKS, and no line of this script moves its jaw: its
-- clip keys the weights, and playing the clip plays them. In a multiplayer
-- game that is the way to have every player see a face: this is a client
-- script, every machine runs it, and each works the same weights out of the
-- same clip.
--
-- The one in the middle is moved by this script: a mood that comes and goes,
-- and a blink every few seconds. What a script sets stays on the machine the
-- script ran on -- which is where a client script runs, and why this is one.
--
-- The one on the right does nothing, and costs what a model without shape
-- keys costs: it is drawn in the same call as anything else of its mesh.
--
-- The busts and their stands are the scene (`content/scenes/face.scene.json`,
-- written by `tools/make_scene.py`); the model is drawn by
-- `tools/make_head.py`. Open the project in the editor, select a bust, and
-- its keys are sliders under Properties.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

local talker = workspace:WaitForChild("Talker") :: MeshPart
local actor = workspace:WaitForChild("Actor") :: MeshPart

Lighting.ClockTime = 10.5
Lighting.GeographicLatitude = 25
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- **A clip's weights.** `Talk` nods the head and keys `JawOpen` and `Smile`.
local talk = (talker:WaitForChild("Animation") :: AnimationPlayer):LoadAnimation("Talk" :: Content)
talk.Looped = true
talk:Play()

-- The actor's head sways; its clip keys no shape key, so they are the
-- script's to set.
local idle = (actor:WaitForChild("Animation") :: AnimationPlayer):LoadAnimation("Idle" :: Content)
idle.Looped = true
idle:Play()

-- **A script's weights.** Seeded, so every run blinks at the same moments.
local random = Random.new(7)
local BlinkTime = 0.14
local untilBlink = 1.2
local blinking = 0
local clock = 0

RunService.PreRender:Connect(function(delta: number)
    clock += delta

    -- Pleased, then cross, and round again: a smile with the brows up, a
    -- frown with them down. A weight of nought is a weight -- the key is
    -- back at rest, and when they all are the bust is drawn as the still one.
    local mood = math.sin(clock * 0.9)
    actor:SetMorphWeight("Smile", math.max(mood, 0))
    actor:SetMorphWeight("BrowUp", math.max(mood, 0) * 0.6)
    actor:SetMorphWeight("BrowDown", math.max(-mood, 0) * 0.9)

    untilBlink -= delta
    if untilBlink <= 0 then
        blinking = BlinkTime
        untilBlink = random:NextNumber(1.4, 4.0)
    end
    if blinking > 0 then
        blinking -= delta
        -- Shut and open again, in fourteen hundredths of a second.
        local through = math.clamp(1 - blinking / BlinkTime, 0, 1)
        actor:SetMorphWeight("Blink", math.sin(through * math.pi))
    else
        actor:SetMorphWeight("Blink", 0)
    end
end)
