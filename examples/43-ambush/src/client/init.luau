--!strict
--[[
	An ambush in an alley (ADR 0204): a guard who is not in the scene steps
	out, says his line and strikes -- and what the sequence could not know
	when it was written, the game tells it as it runs.

	What does each job:

	- **The guard is brought by the sequence.** A `cast` track names a stamp,
	  `content/stamps/guard.stamp.json`, and a name, `Guard`: he is made when
	  the track's first key is reached, the other tracks walk him in and aim
	  the camera at him under that name, and he is gone when the track says
	  so. Nothing in this script makes him.
	- **How hard the blow lands is the game's to say.** The flash on the hero
	  is keyed to `"$Blow"`: the `SequencePlayer`'s attribute of that name,
	  read when the key is used. The script rolls it and sets it; in a match
	  the server would, and every machine would see the same flash.
	- **The camera goes round on one curve** (`"path": "curve"`), and its
	  third key is behind a wall on purpose: `"keepInSight": true` brings it
	  towards the hero until the wall is behind it and not in front.
	- **The cut waits for the line to be over, as it is heard here**
	  (`"afterLine"`): a fifth of a second after the guard's last word, in
	  whichever language the voices are in.
	- **The hand shakes when the blow lands** (`"shake"`): a few centimetres,
	  nine times a second, eased away over the next second and a half.
	- **And this script has the last word on the camera.** `BindCamera` is
	  asked every frame where the camera is to be: it leans the lens in as the
	  guard closes, which no file knows how to measure.
	- **What comes after is written here**, as a table, by `LoadSequence`:
	  the harder the blow, the longer the hero is looked down on.

	Enter or Start: again, with another blow.
]]

local subtitles = require("@engine/subtitles")

local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 20.5
Lighting.GeographicLatitude = 30
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
Lighting.Ambient = Color3.fromRGB(46, 50, 66)
Lighting.OutdoorAmbient = Color3.fromRGB(70, 74, 92)

-- --- The alley ---------------------------------------------------------------------

local function slab(name: string, size: vector, position: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Anchored = true
    part.Parent = workspace
    return part
end

slab("Ground", vector.create(60, 1, 60), vector.create(0, -0.5, 0), Color3.fromRGB(120, 116, 110))
-- The wall the camera's third key is behind.
slab("Wall", vector.create(1, 5, 8), vector.create(4, 2.5, -1), Color3.fromRGB(150, 96, 78))
slab("Crate", vector.create(1.2, 1.2, 1.2), vector.create(-3.4, 0.6, 2.2), Color3.fromRGB(112, 86, 58))

local hero = Instance.new("Part")
hero.Name = "Hero"
hero.Size = vector.create(0.8, 1.8, 0.6)
hero.CFrame = CFrame.lookAt(vector.create(0, 0.9, 0), vector.create(-9, 0.9, -6))
hero:SetMaterialParameter("Color", Color3.fromRGB(232, 228, 220))
hero.Anchored = true
hero.CanCollide = false
hero.Parent = workspace

-- What the blow lights: nothing, until the sequence keys it.
local flash = Instance.new("PointLight")
flash.Name = "Flash"
flash.Color = Color3.fromRGB(255, 90, 60)
flash.Brightness = 0
flash.Range = 7
flash.Parent = hero

-- --- What is on the screen -----------------------------------------------------------

local hud = Instance.new("ScreenGui")
hud.Name = "Hud"
hud.ReferenceHeight = 720
hud.Parent = UIService

local hint = Instance.new("TextLabel")
hint.AnchorPoint = Vector2.new(0.5, 1)
hint.Position = UDim2.new(0.5, 0, 1, -110)
hint.Size = UDim2.new(1, -80, 0, 28)
hint.BackgroundTransparency = 1
hint.TextSize = 18
hint.TextColor = Color3.fromRGB(226, 230, 240)
hint.Parent = hud

subtitles.mount()

-- --- The sequence ----------------------------------------------------------------------

local AmbushFile = "asset://sequences/ambush.sequence.json" :: Content

local ambush = Instance.new("SequencePlayer")
ambush.Name = "Ambush"
ambush.Sequence = AmbushFile
local heroBinding = Instance.new("SequenceBinding")
heroBinding.Name = "Hero"
heroBinding.Target = hero
heroBinding.Parent = ambush
ambush.Parent = workspace

-- The blow: the game's own dice, seeded so a run is a run.
local dice = Random.new(7)
local blow = 0

-- **The last word on the camera.** Handed where the sequence put it, this
-- leans the lens in as the guard closes on the hero -- and keeps count of how
-- near the camera came while its key was behind the wall.
local asked = 0
local nearest = math.huge
ambush:BindCamera(function(cframe: CFrame, fieldOfView: number, time: number)
    asked += 1
    if time > 5 and time < 5.7 then
        nearest = math.min(nearest, vector.magnitude(cframe.Position - hero.Position))
    end
    local guard = ambush:FindFirstChild("Guard") :: SequenceBinding?
    local who = if guard then guard.Target :: Part? else nil
    if who == nil or who.Parent == nil then
        return nil
    end
    local apart = vector.magnitude(who.Position - hero.Position)
    return cframe, fieldOfView - math.clamp(8 - apart, 0, 6)
end)

-- What comes after, written here: the harder the blow, the longer the look.
local function aftermath(strength: number): { [string]: any }
    local long = 1 + strength * 0.25
    return {
        format = "sequence",
        version = 1,
        bindings = { "Hero" },
        tracks = {
            {
                type = "camera",
                keys = {
                    { time = 0, position = { 0.3, 2.6, 1.2 }, relativeTo = "Hero", lookAt = "Hero", cut = true },
                    {
                        time = long,
                        position = { 0.3, 4.2, 1.6 },
                        relativeTo = "Hero",
                        lookAt = "Hero",
                        ease = "sineOut",
                    },
                },
            },
            { type = "fade", keys = { { time = long - 0.6, value = 0 }, { time = long, value = 1 } } },
            { type = "event", keys = { { time = long, name = "Down", data = strength } } },
        },
    }
end

local written = false

local function say()
    hint.Text = LocalizationService:Translate(if ambush.Playing then "hud.playing" else "hud.over")
end

ambush.EventReached:Connect(function(name: string, data: any)
    if name == "Struck" then
        local guard = ambush:FindFirstChild("Guard") :: SequenceBinding?
        local there = guard ~= nil and guard.Target ~= nil and (guard.Target :: Instance).Parent == workspace
        print(`[ambush] struck, blow={blow}, the flash is {flash.Brightness}, the guard is there={there}`)
        print(`[ambush] kept in sight: {math.round(nearest * 10) / 10} m from the hero, with its key behind a wall`)
    elseif name == "Down" then
        print(`[ambush] down, by a sequence the game wrote: {data}`)
    end
end)

ambush.Finished:Connect(function()
    if written then
        print(`[ambush] over; the camera was asked {if asked > 0 then "every frame" else "never"}`)
        say()
        return
    end
    -- The file is over and the guard has gone with it: the rest is the game's.
    print(
        `[ambush] the file is over at {ambush.TimePosition}; the guard is gone={ambush:FindFirstChild("Guard") == nil}`
    )
    written = true
    ambush:LoadSequence(aftermath(blow))
    ambush:Play()
end)

local function again()
    written = false
    nearest = math.huge
    blow = dice:NextInteger(2, 6)
    ambush:SetAttribute("Blow", blow)
    flash.Brightness = 0
    ambush.Sequence = AmbushFile
    ambush.TimePosition = 0
    ambush:Play()
    say()
end

InputService.InputBegan:Connect(function(input: InputObject, processed: boolean)
    if processed then
        return
    end
    if input.KeyCode == Enum.KeyCode.Return or input.KeyCode == Enum.KeyCode.ButtonStart then
        again()
    end
end)

again()
