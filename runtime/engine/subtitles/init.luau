--!strict
--- `@engine/subtitles` -- what is being said, on the screen, in one call (ADR
--- 0200).
---
--- `subtitles.mount()` shows the captions of the sounds this machine can hear
--- at the foot of the screen: who is speaking, in their colour, and the words
--- in the language the player READS -- which need not be the one they hear.
--- Nothing of it is private: every line is a `Caption` from
--- `DialogueService:GetCaptions`, every word comes from
--- `LocalizationService:Translate`, and a game that wants its own subtitles
--- writes them with the same two services.
---
--- **The player decides whether they are shown, and how.**
--- `DialogueService.Subtitles` is theirs -- when needed, always, never -- and
--- so are `SubtitleScale` and `SubtitleBackground`: a size and a background a
--- player can change are what makes a subtitle readable by somebody other
--- than the person who made the game. All three are on the options screen
--- (`@engine/settings`), and are kept with the player's preferences.
---
--- **"When needed" is when what is heard is not what is read**: a voice in
--- another language than the text, a line that has no recording in the
--- player's language, a line nobody has recorded yet.
---
--- Casing follows ADR 0034: `subtitles.mount` is a module function; what it
--- returns is an object, so `display:Unmount()`.

local DialogueService = game:GetService("DialogueService")
local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

local Subtitles = {}

export type Options = {
    --- The screen's `DisplayOrder`. 90 by default: over a game's own screens
    --- and under the options screen.
    DisplayOrder: number?,
    --- An `asset://` typeface. The engine's own when absent.
    Font: string?,
    --- The words, and what is behind them.
    TextColor: Color3?,
    Background: Color3?,
    --- The size of the words at `SubtitleScale` 1, on a screen 720 high. 26.
    TextSize: number?,
    --- How much of the screen's width a line may take, 0 to 1. 0.7.
    Width: number?,
    --- How far from the foot of the screen the last line sits, on a screen
    --- 720 high. 56 -- clear of a game's own bar along the bottom.
    Bottom: number?,
    --- How many captions are shown at once, the newest kept. 3.
    MaxCaptions: number?,
}

export type Display = {
    --- The screen itself. Yours to move in the draw order or to hide.
    Gui: ScreenGui,
    --- False from `Unmount` on.
    IsMounted: boolean,
    --- The lines on the screen now, top to bottom, as plain words -- the
    --- speaker's name and what is said, with no markup. For a test, and for a
    --- game that reads them out.
    GetLines: (self: Display) -> { string },
    --- Takes the screen away and stops listening.
    Unmount: (self: Display) -> (),
}

-- The screen the sizes below are written for; a larger one scales them.
local ReferenceHeight = 720

--- The engine's own words, in the player's language.
local function translate(key: string, arguments: { [string]: any }?): string
    return LocalizationService:Translate(key, arguments)
end

--- What a player wrote, safe inside rich text: a line that says `<b>` shows
--- it, and does not turn the rest of the sentence bold.
local function plain(text: string): string
    return (string.gsub(string.gsub(text, "&", "&amp;"), "<", "&lt;"))
end

--- Puts the display on the screen and returns it. One is what a game wants;
--- a second call makes a second over the first.
function Subtitles.mount(options: Options?): Display
    local settings: Options = options or {}
    local ink = settings.TextColor or Color3.fromRGB(244, 246, 250)
    local behind = settings.Background or Color3.fromRGB(0, 0, 0)
    local textSize = settings.TextSize or 26
    local width = math.clamp(settings.Width or 0.7, 0.2, 1)
    local bottom = settings.Bottom or 56
    local most = math.max(1, settings.MaxCaptions or 3)

    local gui = Instance.new("ScreenGui")
    gui.Name = "Subtitles"
    gui.ReferenceHeight = ReferenceHeight
    gui.DisplayOrder = settings.DisplayOrder or 90
    -- A line that is being said when the scene changes is still being said.
    gui.KeepOnSceneLoad = true

    local column = Instance.new("Frame")
    column.Name = "Lines"
    column.AnchorPoint = Vector2.new(0.5, 1)
    column.Position = UDim2.new(0.5, 0, 1, -bottom)
    column.Size = UDim2.new(width, 0, 0, 0)
    column.AutomaticSize = Enum.AutomaticSize.Y
    column.BackgroundTransparency = 1
    -- Subtitles are read, never pressed: a click goes through to the game.
    column.Active = false
    column.Parent = gui

    local stack = Instance.new("UIListLayout")
    stack.Padding = UDim.new(0, 6)
    stack.HorizontalAlignment = Enum.HorizontalAlignment.Center
    stack.Parent = column

    type Shown = {
        Caption: Caption,
        Frame: Frame,
        Words: TextLabel,
        Plain: string,
    }
    local shown: { Shown } = {}
    local order = 0

    local display = { Gui = gui, IsMounted = true } :: Display

    -- One caption's words: who says it, in their colour, and what they say.
    local function wordsOf(caption: Caption): (string, string)
        local said = LocalizationService:Translate(caption.Text)
        if caption.Speaker == "" then
            return plain(said), said
        end
        local speaker = LocalizationService:Translate(caption.Speaker)
        local marked = `<font color="#{caption.Color:ToHex()}">{plain(speaker)}</font>`
        return translate("engine.subtitles.line", { speaker = marked, text = plain(said) }),
            translate("engine.subtitles.line", { speaker = speaker, text = said })
    end

    local function dress(entry: Shown)
        local scale = DialogueService.SubtitleScale
        entry.Frame.BackgroundTransparency = 1 - DialogueService.SubtitleBackground
        entry.Words.TextSize = math.floor(textSize * scale + 0.5)
        local rich, words = wordsOf(entry.Caption)
        entry.Words.Text = rich
        entry.Plain = words
    end

    local function add(caption: Caption)
        order += 1
        local frame = Instance.new("Frame")
        frame.Name = "Line"
        frame.LayoutOrder = order
        frame.Size = UDim2.new(0, 0, 0, 0)
        frame.AutomaticSize = Enum.AutomaticSize.XY
        frame.BackgroundColor = behind
        frame.Active = false

        local rounded = Instance.new("UICorner")
        rounded.CornerRadius = UDim.new(0, 6)
        rounded.Parent = frame

        local padding = Instance.new("UIPadding")
        padding.PaddingLeft = UDim.new(0, 14)
        padding.PaddingRight = UDim.new(0, 14)
        padding.PaddingTop = UDim.new(0, 6)
        padding.PaddingBottom = UDim.new(0, 6)
        padding.Parent = frame

        local words = Instance.new("TextLabel")
        words.Name = "Words"
        words.BackgroundTransparency = 1
        words.Size = UDim2.new(0, 0, 0, 0)
        words.AutomaticSize = Enum.AutomaticSize.XY
        words.TextColor = ink
        words.RichText = true
        words.TextWrapped = true
        words.Active = false
        if settings.Font then
            words.Font = settings.Font
        end
        words.Parent = frame

        local entry: Shown = { Caption = caption, Frame = frame, Words = words, Plain = "" }
        dress(entry)
        frame.Parent = column
        table.insert(shown, entry)
    end

    -- What is being said and wanted, against what is on the screen: asked
    -- every frame, so a line that ended with its speaker gone -- destroyed,
    -- out of earshot -- comes off without an event for it.
    local function reconcile()
        if not display.IsMounted then
            return
        end
        local wanted: { Caption } = {}
        for _, caption in DialogueService:GetCaptions() do
            if DialogueService:ShowsCaption(caption) then
                table.insert(wanted, caption)
            end
        end
        -- The newest few, when more are speaking than fit.
        while #wanted > most do
            table.remove(wanted, 1)
        end
        for index = #shown, 1, -1 do
            if table.find(wanted, shown[index].Caption) == nil then
                shown[index].Frame:Destroy()
                table.remove(shown, index)
            end
        end
        for _, caption in wanted do
            local known = false
            for _, entry in shown do
                known = known or entry.Caption == caption
            end
            if not known then
                add(caption)
            end
        end
    end

    local function redress()
        for _, entry in shown do
            dress(entry)
        end
    end

    local connections: { Connection } = {
        RunService.Heartbeat:Connect(reconcile),
        -- The language being read changed, or how the player wants them.
        LocalizationService.LocaleChanged:Connect(redress),
        DialogueService:GetPropertyChangedSignal("SubtitleScale"):Connect(redress),
        DialogueService:GetPropertyChangedSignal("SubtitleBackground"):Connect(redress),
    }

    function display.GetLines(_self: Display): { string }
        local lines: { string } = {}
        for _, entry in shown do
            table.insert(lines, entry.Plain)
        end
        return lines
    end

    function display.Unmount(self: Display)
        if not self.IsMounted then
            return
        end
        self.IsMounted = false
        for _, connection in connections do
            connection:Disconnect()
        end
        table.clear(shown)
        gui:Destroy()
    end

    reconcile()
    gui.Parent = UIService
    return display
end

return Subtitles
