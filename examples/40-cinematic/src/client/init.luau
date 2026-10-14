--!strict
--[[
	A cutscene (ADR 0204): two people meet in a room, the camera cuts between
	them, each says a line, the lamp comes up and a door opens -- and the
	whole of it is a file, `content/sequences/meeting.sequence.json`, played
	by the `SequencePlayer` in the scene.

	What does each job:

	- **The sequence** is tracks over seconds: a camera, three lines, a clip,
	  a blink, the lamp's brightness and colour, the door's place and turn,
	  and two events. Open the file: it is written to be read. Open the
	  project in the editor, select `Meeting` and open the Timeline: the same
	  file, with a playhead.
	- **Who it plays on** is the scene's to say: `Meeting` has a
	  `SequenceBinding` child for each name the file expects -- `Anna`,
	  `Bruno`, `Lamp`, `Door` -- each pointing at an instance.
	- **The camera is taken as a view.** `workspace.CurrentCamera` is never
	  written: this script does not hand the camera over or take it back, and
	  when the sequence ends the picture is the game's own camera again.
	- **The lines** are `DialogueService`'s, said in each player's own voice
	  language and read in their own text language, with mouths that follow
	  them. `V` and `T` change the two. The sequence is as long in one
	  language as in the other: a line's slot is its longest recording.
	- **Skipping** lands the world where the sequence would have left it: the
	  lamp up, the door open, and the event that says so fired with `skipped`
	  true. `Controls = Suspend`, set on the player in the scene, holds a
	  game's own movement while it plays; the keys below still arrive.

	All this script does is play it, skip it, pause it and say what it hears.
]]

local subtitles = require("@engine/subtitles")

local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local UIService = game:GetService("UIService")

-- Evening: a room a lamp can be seen to light.
Lighting.ClockTime = 19.4
Lighting.GeographicLatitude = 25
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

local meeting = workspace:WaitForChild("Meeting") :: SequencePlayer

-- The busts sway by themselves: the game's own animation, which goes on
-- under whatever clip the sequence plays over it and is there when that ends.
for _, name in { "Anna", "Bruno" } do
    local bust = (workspace:WaitForChild(name) :: Model):WaitForChild("Bust") :: MeshPart
    local idle = (bust:WaitForChild("Animation") :: AnimationPlayer):LoadAnimation("Idle" :: Content)
    idle.Looped = true
    idle:Play()
end

-- --- What is on the screen ---------------------------------------------------------

local Text = Color3.fromRGB(226, 230, 240)

local hud = Instance.new("ScreenGui")
hud.Name = "Hud"
hud.ReferenceHeight = 720
hud.Parent = UIService

local function line(y: number, size: number): TextLabel
    local label = Instance.new("TextLabel")
    label.AnchorPoint = Vector2.new(0.5, 0)
    label.Position = UDim2.new(0.5, 0, 0, y)
    label.Size = UDim2.new(1, -80, 0, size + 8)
    label.BackgroundTransparency = 1
    label.TextSize = size
    label.TextColor = Text
    label.Parent = hud
    return label
end

local hint = line(16, 18)
local languages = line(44, 15)
local heard = line(70, 15)

-- What was last said about how it ended, and the last event's words.
local skipped = false
local lastEvent: string? = nil

local function caption()
    if meeting.Playing then
        hint.Text = LocalizationService:Translate("hud.playing")
    elseif meeting.TimePosition > 0 and meeting.TimePosition < meeting.Length then
        hint.Text = LocalizationService:Translate("hud.paused")
    else
        hint.Text = LocalizationService:Translate(if skipped then "hud.skipped" else "hud.over")
    end
    languages.Text = LocalizationService:Translate(
        "hud.voice",
        { voice = LocalizationService.VoiceLocale, text = LocalizationService.Locale }
    )
    heard.Text = if lastEvent then LocalizationService:Translate("hud.event", { name = lastEvent }) else ""
end

LocalizationService.LocaleChanged:Connect(caption)
LocalizationService.VoiceLocaleChanged:Connect(caption)

-- The lines, at the foot of the screen, as the player wants them shown.
subtitles.mount()

-- --- What the sequence says --------------------------------------------------------

-- An event is a name and whatever the file wrote beside it. One fired by a
-- skip says so: a handler that opens a door opens it, and one that would
-- only have made a noise can stay quiet.
meeting.EventReached:Connect(function(name: string, data: any, wasSkipped: boolean)
    lastEvent = if wasSkipped then `{name} (skipped)` else name
    print(`[cinematic] event {name} data={data} skipped={wasSkipped}`)
    caption()
end)

-- One signal for "the cutscene is over", however it came to be.
meeting.Finished:Connect(function(wasSkipped: boolean)
    skipped = wasSkipped
    print(`[cinematic] finished skipped={wasSkipped} at {meeting.TimePosition}`)
    caption()
end)

-- --- The keys ----------------------------------------------------------------------

local keys = Instance.new("InputContext")
keys.Name = "Cinematic"
keys.Parent = workspace

local function action(name: string, bindings: { EnumKeyCode }, pressed: () -> ())
    local made = Instance.new("InputAction")
    made.Name = name
    made.Parent = keys
    for _, code in bindings do
        local binding = Instance.new("InputBinding")
        binding.KeyCode = code
        binding.Parent = made
    end
    made.Pressed:Connect(pressed)
end

action("Skip", { Enum.KeyCode.Space, Enum.KeyCode.ButtonSouth }, function()
    if meeting.Playing or meeting.TimePosition > 0 and meeting.TimePosition < meeting.Length then
        meeting:Skip()
    end
end)

action("Pause", { Enum.KeyCode.P }, function()
    if meeting.Playing then
        meeting:Pause()
    elseif meeting.TimePosition < meeting.Length then
        meeting:Play()
    end
    caption()
end)

action("Again", { Enum.KeyCode.Return, Enum.KeyCode.ButtonStart }, function()
    if meeting.Playing then
        return
    end
    -- From the start: at its end, `Play` begins again. The file keys the lamp
    -- and the door from nought, so they are put back by the sequence itself.
    meeting.TimePosition = 0
    skipped = false
    lastEvent = nil
    meeting:Play()
    caption()
end)

action("Voice", { Enum.KeyCode.V }, function()
    -- Each language this machine can play, going round. Heard at the next
    -- line: what is being said finishes in the language it began in.
    local voices = LocalizationService:GetVoiceLocales()
    local at = table.find(voices, LocalizationService.VoiceLocale) or 0
    LocalizationService.VoiceLocale = voices[at % #voices + 1]
end)

action("Words", { Enum.KeyCode.T }, function()
    local locales = LocalizationService:GetLocales()
    local at = table.find(locales, LocalizationService.Locale) or 0
    LocalizationService.Locale = locales[at % #locales + 1]
end)

-- --- And it plays ------------------------------------------------------------------

task.wait(0.5)
meeting:Play()
caption()
