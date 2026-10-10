--!strict
--[[
	Dubbing (ADR 0200): three speakers who say six lines, in English or in
	Brazilian Portuguese, with subtitles and mouths that move.

	What does each job:

	- **The lines** are `content/dialogue/booth.lines.json`: who says each, in
	  what colour, and where its recording is. `DialogueService:Say` makes
	  the sound, gives it its caption, plays it and takes it away.
	- **The two languages** are folders. `content/voice/booth/anna_1.wav` is
	  the line in the default language; `content/l10n/pt-BR/voice/booth/
	  anna_1.wav` is the same line in Portuguese, and is found by itself when
	  `LocalizationService.VoiceLocale` is `pt-BR`. No line of this script
	  names a language's file.
	- **The voice's language is not the text's.** The two buttons change
	  each: hear Portuguese and read English, or the other way round.
	- **Subtitles** are `require("@engine/subtitles").mount()`. Left as it
	  comes -- "when needed" -- they show when what is heard is not what is
	  read: a voice in the other language, the line nobody dubbed
	  (`bruno_2`), the line nobody recorded (`clara_2`, which is silent and
	  lasts as long as it takes to read).
	- **The options screen** is `require("@engine/settings").open()`, on its
	  Audio page: volumes, both languages, and how subtitles are shown.
	- **The mouths** are a `LipSync` under each bust, in the scene, and no
	  line of this script moves one. Anna's follows the track beside each
	  recording -- `anna_1.visemes.json`, one per language; Bruno's follows
	  three bands of the sound; Clara's follows loudness alone.

	The recordings are a synthesizer reading each line's letters
	(`tools/make_voices.py`), not voices: they are there so the example has
	two languages to play.
]]

local settings = require("@engine/settings")
local subtitles = require("@engine/subtitles")

local DialogueService = game:GetService("DialogueService")
local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local UIService = game:GetService("UIService")

Lighting.ClockTime = 10.5
Lighting.GeographicLatitude = 25
Lighting.FogStart = 100000
Lighting.FogEnd = 200000

-- --- The speakers ------------------------------------------------------------------

type Speaker = {
    Bust: MeshPart,
    -- The catalog key of the way this one's mouth is moved.
    Way: string,
}

local function speaker(name: string, way: string): Speaker
    local bust = (workspace:WaitForChild(name) :: Model):WaitForChild("Bust") :: MeshPart
    -- The head sways; the clip keys no shape key, so the mouth is the voice's.
    local idle = (bust:WaitForChild("Animation") :: AnimationPlayer):LoadAnimation("Idle" :: Content)
    idle.Looped = true
    idle:Play()
    return { Bust = bust, Way = way }
end

local Speakers: { [string]: Speaker } = {
    anna = speaker("Anna", "way.track"),
    bruno = speaker("Bruno", "way.bands"),
    clara = speaker("Clara", "way.loudness"),
}

-- --- What is on the screen ---------------------------------------------------------

local Raised = Color3.fromRGB(44, 50, 68)
local Text = Color3.fromRGB(226, 230, 240)

-- Under each bust, how its mouth is moved.
local tags: { [string]: TextLabel } = {}
for name, who in Speakers do
    local tag = Instance.new("BillboardGui")
    tag.Size = UDim2.new(0, 300, 0, 30)
    tag.WorldOffset = vector.create(0, -0.22, 0.34)
    tag.Parent = who.Bust

    local label = Instance.new("TextLabel")
    label.Size = UDim2.fromScale(1, 1)
    label.BackgroundColor = Color3.fromRGB(10, 12, 18)
    label.BackgroundTransparency = 0.35
    label.TextColor = Text
    label.TextSize = 17
    label.Parent = tag
    local rounded = Instance.new("UICorner")
    rounded.CornerRadius = UDim.new(0, 6)
    rounded.Parent = label
    tags[name] = label
end

local hud = Instance.new("ScreenGui")
hud.Name = "Hud"
hud.ReferenceHeight = 720
hud.Parent = UIService

local function button(order: number): TextButton
    local made = Instance.new("TextButton")
    made.AnchorPoint = Vector2.new(1, 0)
    made.Position = UDim2.new(1, -20, 0, 20 + (order - 1) * 56)
    made.Size = UDim2.fromOffset(300, 46)
    made.TextSize = 20
    made.TextColor = Text
    made.BackgroundColor = Raised
    local rounded = Instance.new("UICorner")
    rounded.CornerRadius = UDim.new(0, 8)
    rounded.Parent = made
    made.Parent = hud
    return made
end

local optionsButton = button(1)
local voiceButton = button(2)
local textButton = button(3)

local hint = Instance.new("TextLabel")
hint.AnchorPoint = Vector2.new(0.5, 0)
hint.Position = UDim2.new(0.5, 0, 0, 18)
hint.Size = UDim2.new(1, -700, 0, 24)
hint.BackgroundTransparency = 1
hint.TextSize = 16
hint.TextColor = Text
hint.Parent = hud

-- Every word of this script's on the screen, said again whenever either
-- language changes.
local function caption()
    optionsButton.Text = LocalizationService:Translate("menu.options")
    voiceButton.Text = if LocalizationService.VoiceFollowsLocale
        then LocalizationService:Translate("menu.voice_follows")
        else LocalizationService:Translate("menu.voice", { locale = LocalizationService.VoiceLocale })
    textButton.Text = LocalizationService:Translate("menu.text", { locale = LocalizationService.Locale })
    hint.Text = LocalizationService:Translate("menu.hint")
    for name, label in tags do
        label.Text = LocalizationService:Translate(Speakers[name].Way)
    end
end
caption()
LocalizationService.LocaleChanged:Connect(caption)
LocalizationService.VoiceLocaleChanged:Connect(caption)

-- The lines, at the foot of the screen, as the player wants them shown.
subtitles.mount()

-- --- The buttons -------------------------------------------------------------------

voiceButton.Activated:Connect(function()
    -- "The same as the text" -- written as no language at all -- and then
    -- each language this machine can play, going round. A change is heard at
    -- the next line: what is being said finishes in the language it began in.
    local voices: { string } = { "" }
    for _, locale in LocalizationService:GetVoiceLocales() do
        table.insert(voices, locale)
    end
    local now = if LocalizationService.VoiceFollowsLocale then "" else LocalizationService.VoiceLocale
    local at = table.find(voices, now) or 1
    LocalizationService.VoiceLocale = voices[at % #voices + 1]
    -- Following again changes the button's words and maybe nothing heard.
    caption()
end)

textButton.Activated:Connect(function()
    local locales = LocalizationService:GetLocales()
    local at = table.find(locales, LocalizationService.Locale) or 0
    LocalizationService.Locale = locales[at % #locales + 1]
end)

local open: settings.Screen? = nil

local function toggle()
    if open then
        open:Close()
        return
    end
    local screen = settings.open()
    screen:ShowPage("Audio")
    open = screen
    screen.Closed:Connect(function()
        open = nil
        caption()
    end)
end

optionsButton.Activated:Connect(toggle)

local menu = Instance.new("InputContext")
menu.Name = "Menu"
menu.Parent = workspace

local toggleAction = Instance.new("InputAction")
toggleAction.Name = "Options"
toggleAction.Parent = menu

local escape = Instance.new("InputBinding")
escape.KeyCode = Enum.KeyCode.Escape
escape.Parent = toggleAction

local start = Instance.new("InputBinding")
start.KeyCode = Enum.KeyCode.ButtonStart
start.Parent = toggleAction

toggleAction.Pressed:Connect(toggle)

-- --- The conversation --------------------------------------------------------------

local Lines = { "anna_1", "bruno_1", "clara_1", "anna_2", "bruno_2", "clara_2" }

-- Round and round. A line is said from its speaker's bust: heard from there,
-- and under the model whose mouth it moves.
task.spawn(function()
    task.wait(0.6)
    while true do
        for _, line in Lines do
            local who = Speakers[string.match(line, "^%a+") :: string]
            local sound = DialogueService:Say(`booth.{line}`, who.Bust)
            -- As long in one language as in the other: a line lasts as long
            -- as its longest recording, so what follows it starts at the
            -- same moment whatever anybody hears.
            sound.Ended:Wait()
            task.wait(0.35)
        end
        task.wait(1.2)
    end
end)
