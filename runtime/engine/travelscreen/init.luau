--!strict
--- `@engine/travelscreen` -- a loading screen between two worlds, in one call
--- (ADR 0222).
---
--- `travelscreen.mount()` covers the screen when this machine is sent to
--- another world -- by the front door, or by the world it is in -- with the
--- world's name and a bar that moves while it loads, and takes it away when
--- that world has taken this machine (`NetworkService.Connected`). Nothing of
--- it is private: a game that wants its own connects `TravelStarted` and
--- `Connected` and draws it its own way.
---
--- Mount it where it lives across worlds: a `GlobalScriptService.Client`
--- script, which the hop does not stop.
---
--- Casing follows ADR 0034: `travelscreen.mount` is a module function; what
--- it returns is an object, so `display:Unmount()`.

local LocalizationService = game:GetService("LocalizationService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

local TravelScreen = {}

export type Options = {
    --- The screen's `DisplayOrder`. 200 by default: over everything a game
    --- draws.
    DisplayOrder: number?,
    --- An `asset://` typeface. The engine's own when absent.
    Font: string?,
    TextColor: Color3?,
    Background: Color3?,
    --- The bar's colour.
    Accent: Color3?,
}

export type Display = {
    --- The screen itself. Yours to restyle or to hide.
    Gui: ScreenGui,
    --- False from `Unmount` on.
    IsMounted: boolean,
    --- Whether it covers the screen now.
    IsShowing: (self: Display) -> boolean,
    --- What it says now, as plain words.
    GetText: (self: Display) -> string,
    --- Takes the screen away and stops listening.
    Unmount: (self: Display) -> (),
}

local ReferenceHeight = 720
-- How long the bar takes to cross, in seconds.
local SweepSeconds = 1.2

--- Puts the screen in place, hidden, and returns it.
function TravelScreen.mount(options: Options?): Display
    local settings: Options = options or {}
    local ink = settings.TextColor or Color3.fromRGB(240, 236, 226)
    local behind = settings.Background or Color3.fromRGB(14, 13, 12)
    local accent = settings.Accent or Color3.fromRGB(214, 168, 92)

    local gui = Instance.new("ScreenGui")
    gui.Name = "TravelScreen"
    gui.ReferenceHeight = ReferenceHeight
    gui.DisplayOrder = settings.DisplayOrder or 200
    -- The hop changes the scene under it; it stays until the next world is in.
    gui.KeepOnSceneLoad = true
    gui.Enabled = false

    local card = Instance.new("Frame")
    card.Name = "Card"
    card.Size = UDim2.new(1, 0, 1, 0)
    card.BackgroundColor = behind
    card.BackgroundTransparency = 0
    -- It covers the game, and so takes its clicks while it does.
    card.Active = true
    card.Parent = gui

    local words = Instance.new("TextLabel")
    words.Name = "Words"
    words.AnchorPoint = Vector2.new(0.5, 0.5)
    words.Position = UDim2.new(0.5, 0, 0.5, -24)
    words.Size = UDim2.new(0.8, 0, 0, 48)
    words.BackgroundTransparency = 1
    words.TextColor = ink
    words.TextSize = 32
    words.RichText = false
    words.Active = false
    if settings.Font then
        words.Font = settings.Font
    end
    words.Parent = card

    local track = Instance.new("Frame")
    track.Name = "Track"
    track.AnchorPoint = Vector2.new(0.5, 0.5)
    track.Position = UDim2.new(0.5, 0, 0.5, 28)
    track.Size = UDim2.new(0.4, 0, 0, 6)
    track.BackgroundColor = ink
    track.BackgroundTransparency = 0.85
    track.ClipsDescendants = true
    track.Active = false
    track.Parent = card

    local bar = Instance.new("Frame")
    bar.Name = "Bar"
    bar.Size = UDim2.new(0.3, 0, 1, 0)
    bar.BackgroundColor = accent
    bar.Active = false
    bar.Parent = track

    local display = { Gui = gui, IsMounted = true } :: Display
    local showing = false
    local swept = 0

    local function show(world: string)
        words.Text = LocalizationService:Translate("engine.travelscreen.going", { world = world })
        swept = 0
        showing = true
        gui.Enabled = true
    end

    local function hide()
        showing = false
        gui.Enabled = false
    end

    -- **In line at a full game's door** (ADR 0232): the place, and the
    -- minutes when they are known.
    local function queued(position: number, estimateSeconds: number?)
        if estimateSeconds then
            words.Text = LocalizationService:Translate("engine.travelscreen.queued_estimate", {
                position = position,
                minutes = math.max(1, math.ceil(estimateSeconds / 60)),
            })
        else
            words.Text = LocalizationService:Translate("engine.travelscreen.queued", { position = position })
        end
        showing = true
        gui.Enabled = true
    end

    -- **The bar moves while the world loads**: how long a hop takes is not
    -- known ahead, so it sweeps rather than fills.
    local function sweep(dt: number)
        if not showing then
            return
        end
        swept = (swept + dt / SweepSeconds) % 1
        bar.Position = UDim2.new(swept * 1.3 - 0.3, 0, 0, 0)
    end

    local connections: { Connection } = {
        NetworkService.TravelStarted:Connect(show),
        NetworkService.Queued:Connect(queued),
        NetworkService.Connected:Connect(hide),
        -- A hop that fails ends on the machine's own world: nothing to cover.
        NetworkService.Disconnected:Connect(hide),
        NetworkService.JoinFailed:Connect(hide),
        RunService.Heartbeat:Connect(sweep),
    }

    function display.IsShowing(_self: Display): boolean
        return showing
    end

    function display.GetText(_self: Display): string
        return words.Text
    end

    function display.Unmount(self: Display)
        if not self.IsMounted then
            return
        end
        self.IsMounted = false
        for _, connection in connections do
            connection:Disconnect()
        end
        gui:Destroy()
    end

    gui.Parent = UIService
    return display
end

return TravelScreen
