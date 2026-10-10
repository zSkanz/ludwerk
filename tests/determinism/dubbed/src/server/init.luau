--!strict
--- A line in three languages of three lengths (ADR 0200), said over fifteen
--- seconds -- and replayed heard in each of them, against one trace.
---
--- **What a player hears is theirs and no tick's.** The default language's
--- recording is half a second, the Portuguese one nine tenths, the Japanese
--- one three tenths; the line is nine tenths long whoever listens, because
--- that is its longest language. Everything below hangs something the hash
--- reads on WHEN a sound ends -- a part where the tick count says, the next
--- line said from its `Ended` -- so a build in which a length followed the
--- voice language moves the trace at the first checkpoint after a line.
---
--- The assertions are `error` calls, which fail a replay.

local AudioService = game:GetService("AudioService")
local DialogueService = game:GetService("DialogueService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local ticks = 0
local marks = 0

-- A part at a place the tick count decides: where a sound ended is in the
-- world, and so in its hash.
local function mark(what: string)
    marks += 1
    local part = Instance.new("Part")
    part.Name = `{what}{marks}`
    part.Anchored = true
    part.Position = vector.create(ticks, marks, 0)
    part.Parent = Workspace
end

-- A sound of the game's own, played and played again from its `Ended`.
local jingle = Instance.new("Sound")
jingle.Name = "Jingle"
jingle.Content = "asset://voice/line.wav"
jingle.Parent = Workspace
local rounds = 0
jingle.Ended:Connect(function()
    mark("Jingle")
    rounds += 1
    if rounds < 4 then
        jingle:Play()
    end
end)
jingle:Play()

-- One with a caption and no recording in any language: it lasts its caption.
local unrecorded = Instance.new("Sound")
unrecorded.Name = "Unrecorded"
unrecorded.Content = "asset://voice/nobody_recorded_this.wav"
local words = Instance.new("Caption")
words.Text = "twelve chars"
words.Parent = unrecorded
unrecorded.Parent = Workspace
unrecorded.Ended:Connect(function()
    mark("Unrecorded")
end)
unrecorded:Play()

-- Lines said, each from the end of the one before.
local said = 0
local function sayNext()
    said += 1
    if said > 6 then
        return
    end
    local line = if said % 2 == 1 then "scene.recorded" else "scene.written"
    local sound = DialogueService:Say(line)
    sound.Ended:Connect(function()
        mark("Line")
        sayNext()
    end)
end
sayNext()

RunService.Heartbeat:Connect(function()
    ticks += 1
    if ticks == 2 then
        -- The longest language's nine tenths, whatever is being heard.
        if math.abs(jingle.TimeLength - 0.9) > 0.001 then
            error(`the line is {jingle.TimeLength} s long; its longest language is 0.9`)
        end
        -- A second and a half and six hundredths for each of twelve characters.
        if math.abs(unrecorded.TimeLength - 2.22) > 0.001 then
            error(`a caption of twelve characters lasts {unrecorded.TimeLength} s, not 2.22`)
        end
    end
    if ticks == 120 then
        -- Played once and gone when it has been (D621).
        AudioService:PlayLocal("asset://voice/line.wav")
    end
    if ticks == 880 then
        if rounds ~= 4 then
            error(`the jingle ended {rounds} times in fifteen seconds, not 4`)
        end
        if said < 7 then
            error(`only {said - 1} lines were said`)
        end
        if #AudioService:GetChildren() ~= 0 then
            error("a sound played once is still a child of AudioService")
        end
    end
end)
