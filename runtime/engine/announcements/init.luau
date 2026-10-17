--!strict
--- `@engine/announcements` -- what a server's operator says, across the top of
--- the screen, in one call (ADR 0218).
---
--- `announcements.mount()` shows each `NetworkService.Announced` -- the
--- console's `say`, a `stop` counting down -- for a few seconds, the newest
--- under the ones before. Nothing of it is private: a game that wants its own
--- connects `Announced` and draws it its own way.
---
--- Casing follows ADR 0034: `announcements.mount` is a module function; what
--- it returns is an object, so `display:Unmount()`.

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

local Announcements = {}

export type Options = {
    --- The screen's `DisplayOrder`. 95 by default: over a game's own screens
    --- and subtitles, under the options screen.
    DisplayOrder: number?,
    --- How long each is shown, in seconds. 6.
    Seconds: number?,
    --- How many are shown at once, the newest kept. 3.
    MaxLines: number?,
    --- An `asset://` typeface. The engine's own when absent.
    Font: string?,
    TextColor: Color3?,
    Background: Color3?,
    --- The size of the words on a screen 720 high. 24.
    TextSize: number?,
}

export type Display = {
    --- The screen itself. Yours to move in the draw order or to hide.
    Gui: ScreenGui,
    --- False from `Unmount` on.
    IsMounted: boolean,
    --- What is on the screen now, oldest first.
    GetLines: (self: Display) -> { string },
    --- Takes the screen away and stops listening.
    Unmount: (self: Display) -> (),
}

local ReferenceHeight = 720

--- Puts the display on the screen and returns it.
function Announcements.mount(options: Options?): Display
    local settings: Options = options or {}
    local seconds = math.max(0.5, settings.Seconds or 6)
    local most = math.max(1, settings.MaxLines or 3)
    local ink = settings.TextColor or Color3.fromRGB(250, 246, 232)
    local behind = settings.Background or Color3.fromRGB(24, 22, 18)
    local textSize = settings.TextSize or 24

    local gui = Instance.new("ScreenGui")
    gui.Name = "Announcements"
    gui.ReferenceHeight = ReferenceHeight
    gui.DisplayOrder = settings.DisplayOrder or 95
    -- What was said is still said after the scene changes.
    gui.KeepOnSceneLoad = true

    local column = Instance.new("Frame")
    column.Name = "Lines"
    column.AnchorPoint = Vector2.new(0.5, 0)
    column.Position = UDim2.new(0.5, 0, 0, 24)
    column.Size = UDim2.new(0.6, 0, 0, 0)
    column.AutomaticSize = Enum.AutomaticSize.Y
    column.BackgroundTransparency = 1
    -- Read, never pressed: a click goes through to the game.
    column.Active = false
    column.Parent = gui

    local stack = Instance.new("UIListLayout")
    stack.Padding = UDim.new(0, 6)
    stack.HorizontalAlignment = Enum.HorizontalAlignment.Center
    stack.Parent = column

    type Shown = {
        Frame: Frame,
        Text: string,
        Left: number,
    }
    local shown: { Shown } = {}
    local order = 0
    local display = { Gui = gui, IsMounted = true } :: Display

    local function add(text: string)
        order += 1
        local frame = Instance.new("Frame")
        frame.Name = "Line"
        frame.LayoutOrder = order
        frame.Size = UDim2.new(0, 0, 0, 0)
        frame.AutomaticSize = Enum.AutomaticSize.XY
        frame.BackgroundColor = behind
        frame.BackgroundTransparency = 0.15
        frame.Active = false

        local rounded = Instance.new("UICorner")
        rounded.CornerRadius = UDim.new(0, 6)
        rounded.Parent = frame

        local padding = Instance.new("UIPadding")
        padding.PaddingLeft = UDim.new(0, 16)
        padding.PaddingRight = UDim.new(0, 16)
        padding.PaddingTop = UDim.new(0, 8)
        padding.PaddingBottom = UDim.new(0, 8)
        padding.Parent = frame

        local words = Instance.new("TextLabel")
        words.Name = "Words"
        words.BackgroundTransparency = 1
        words.Size = UDim2.new(0, 0, 0, 0)
        words.AutomaticSize = Enum.AutomaticSize.XY
        words.TextColor = ink
        words.TextSize = textSize
        words.TextWrapped = true
        -- The operator's words, shown as written: never markup.
        words.RichText = false
        words.Text = text
        words.Active = false
        if settings.Font then
            words.Font = settings.Font
        end
        words.Parent = frame

        frame.Parent = column
        table.insert(shown, { Frame = frame, Text = text, Left = seconds })
        while #shown > most do
            shown[1].Frame:Destroy()
            table.remove(shown, 1)
        end
    end

    local function age(dt: number)
        for index = #shown, 1, -1 do
            local entry = shown[index]
            entry.Left -= dt
            if entry.Left <= 0 then
                entry.Frame:Destroy()
                table.remove(shown, index)
            end
        end
    end

    local connections: { Connection } = {
        NetworkService.Announced:Connect(add),
        RunService.Heartbeat:Connect(age),
    }

    function display.GetLines(_self: Display): { string }
        local lines: { string } = {}
        for _, entry in shown do
            table.insert(lines, entry.Text)
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

    gui.Parent = UIService
    return display
end

return Announcements
