--!strict
--- `@engine/settings` -- an options screen in one call (ADR 0147, section 7).
---
--- `settings.open()` puts a screen over the game with the graphics and display
--- settings a player expects, and nothing of it is private: every row reads and
--- writes `GraphicsService`, every word comes from
--- `LocalizationService:Translate`, and a game that wants its own menu writes
--- one with the same two services.
---
--- **What a row does when it is stepped.** A quality row writes at once, so the
--- player sees the picture change behind the menu. The three that move the
--- window -- its mode, its resolution and its monitor -- are held until
--- **Apply**: a window that changed mode at every step through a list would be
--- a window nobody could read the list on. Apply writes what is held and keeps
--- everything with `SaveAsync`. Revert, and leaving without applying, put back
--- what was last kept.
---
--- **It shows what this build draws by.** A setting `IsApplied` says nothing
--- reads yet has no row, and a handheld has no window to arrange, so its
--- display page is the frame rate alone.
---
--- **A mouse, a finger, a gamepad or the arrow keys drive it**, because every
--- control is a button: the selection walks them (ADR 0128), and a row the
--- selection reaches is scrolled into sight.
---
--- Casing follows ADR 0034: `settings.open` is a module function; what it
--- returns is an object, so `screen:Close()`.

local GraphicsService = game:GetService("GraphicsService")
local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

local Settings = {}

export type Theme = {
    --- What dims the game behind the window, and by how much it lets it through.
    Backdrop: Color3?,
    BackdropTransparency: number?,
    --- The window.
    Panel: Color3?,
    --- A button, and the box a value sits in.
    Raised: Color3?,
    --- The current page, a setting that is on, and Apply while there is
    --- something to apply.
    Accent: Color3?,
    Text: Color3?,
    --- A caption beside its value.
    Dim: Color3?,
    --- An `asset://` typeface. The engine's own when absent.
    Font: string?,
    CornerRadius: number?,
}

export type Options = {
    Theme: Theme?,
    --- Settings to leave out, by their `GraphicsService` name: `{ "MotionBlur" }`.
    Hide: { string }?,
    --- The screen's `DisplayOrder`. 100 by default: over a game's own screens.
    DisplayOrder: number?,
    --- The window's heading, already in the player's language. The engine's
    --- own word for "Options" when absent.
    Title: string?,
}

export type Page = "Graphics" | "Display"

export type Screen = {
    --- The screen itself. Yours to move in the draw order or to hide.
    Gui: ScreenGui,
    --- False from `Close` on.
    IsOpen: boolean,
    --- Whether something was changed that Apply has not kept yet.
    IsDirty: boolean,
    --- Fired once, when the screen is closed -- by its Back button or by `Close`.
    Closed: Signal<>,
    --- Steps the row of `setting` one choice down (`-1`) or up (`1`), as its
    --- two buttons do. Returns whether anything changed. Raises for a setting
    --- the screen has no row for.
    Step: (self: Screen, setting: string, direction: number) -> boolean,
    --- The words a row shows for its value, or nil when the screen has no
    --- such row.
    GetValueText: (self: Screen, setting: string) -> string?,
    ShowPage: (self: Screen, page: Page) -> (),
    --- Writes what is held, and keeps everything. **Yields**, as `SaveAsync`
    --- does; returns what it returned.
    Apply: (self: Screen) -> boolean,
    --- Back to what was last kept. **Yields**, as `LoadAsync` does.
    Revert: (self: Screen) -> (),
    --- The project's defaults, shown and drawn by; kept only by Apply.
    ResetToDefaults: (self: Screen) -> (),
    --- Closes the screen. What was not applied is put back.
    Close: (self: Screen) -> (),
}

type Kind = "Preset" | "Toggle" | "Choice" | "Steps" | "Resolution" | "Monitor"

type Row = {
    Setting: string,
    Page: Page,
    -- The catalog key of the row's caption.
    Caption: string,
    Kind: Kind,
    -- A choice's items in the order they are stepped through, and the key
    -- each item's name is added to in lower case.
    Items: { any }?,
    ItemKey: string?,
    -- A number's stops, ascending; the key its value is shown through, what
    -- the value is multiplied by first, and the key shown for zero instead.
    Steps: { number }?,
    Format: string?,
    Scale: number?,
    Zero: string?,
    -- Held until Apply rather than written at once.
    Held: boolean?,
    -- Only where there is a window to arrange.
    Desktop: boolean?,
}

type Built = {
    Row: Row,
    Frame: Frame,
    Caption: TextLabel,
    -- Where the value's words go: a label between two buttons, or the one
    -- button of a toggle.
    Value: TextLabel | TextButton,
}

-- The levels a player steps through: the machine's own first, and then the
-- four from the least up. `Custom` is a reading and not a choice.
local Levels: { EnumGraphicsQuality } = {
    Enum.GraphicsQuality.Auto,
    Enum.GraphicsQuality.Low,
    Enum.GraphicsQuality.Medium,
    Enum.GraphicsQuality.High,
    Enum.GraphicsQuality.Ultra,
}

local Percent = "engine.settings.value.percent"
local QualityScale = { 0.5, 0.75, 1, 1.5, 2 }

local function toggle(setting: string, caption: string, page: Page, desktop: boolean?): Row
    return { Setting = setting, Page = page, Caption = caption, Kind = "Toggle", Desktop = desktop }
end

local function choice(setting: string, caption: string, items: { any }, itemKey: string): Row
    return { Setting = setting, Page = "Graphics", Caption = caption, Kind = "Choice", Items = items, ItemKey = itemKey }
end

local function scale(setting: string, caption: string, steps: { number }): Row
    return {
        Setting = setting,
        Page = "Graphics",
        Caption = caption,
        Kind = "Steps",
        Steps = steps,
        Format = Percent,
        Scale = 100,
    }
end

-- Every row there could be, in the order they are shown. Which of them a
-- screen has is decided when it opens.
local Rows: { Row } = {
    { Setting = "QualityLevel", Page = "Graphics", Caption = "engine.graphics.setting.quality_level", Kind = "Preset" },
    -- With the scales FSR 2 is tuned at (ADR 0164): a third, a half, 0.59, 0.67.
    scale("RenderScale", "engine.graphics.setting.render_scale", { 1 / 3, 0.5, 0.59, 0.67, 0.8, 0.9, 1 }),
    choice(
        "AntiAliasing",
        "engine.graphics.setting.anti_aliasing",
        Enum.AntiAliasingMode:GetEnumItems(),
        "engine.graphics.choice.anti_aliasing."
    ),
    choice(
        "Upscaling",
        "engine.graphics.setting.upscaling",
        Enum.UpscalingMode:GetEnumItems(),
        "engine.graphics.choice.upscaling."
    ),
    scale("Sharpness", "engine.graphics.setting.sharpness", { 0, 0.2, 0.4, 0.6, 0.8, 1 }),
    choice(
        "ShadowQuality",
        "engine.graphics.setting.shadow_quality",
        Enum.ShadowQuality:GetEnumItems(),
        "engine.graphics.choice.shadow_quality."
    ),
    {
        Setting = "ShadowDistance",
        Page = "Graphics",
        Caption = "engine.graphics.setting.shadow_distance",
        Kind = "Steps",
        Steps = { 30, 60, 120, 240, 480 },
        Format = "engine.settings.value.metres",
    },
    choice(
        "TextureQuality",
        "engine.graphics.setting.texture_quality",
        Enum.TextureQuality:GetEnumItems(),
        "engine.graphics.choice.texture_quality."
    ),
    scale("ViewDistance", "engine.graphics.setting.view_distance", QualityScale),
    scale("TerrainDetail", "engine.graphics.setting.terrain_detail", QualityScale),
    scale("FoliageDensity", "engine.graphics.setting.foliage_density", { 0, 0.25, 0.5, 0.75, 1 }),
    toggle("AmbientOcclusion", "engine.graphics.setting.ambient_occlusion", "Graphics"),
    toggle("ContactShadows", "engine.graphics.setting.contact_shadows", "Graphics"),
    toggle("FoliageDecals", "engine.graphics.setting.foliage_decals", "Graphics"),
    toggle("Bloom", "engine.graphics.setting.bloom", "Graphics"),
    toggle("DepthOfField", "engine.graphics.setting.depth_of_field", "Graphics"),
    toggle("SunRays", "engine.graphics.setting.sun_rays", "Graphics"),
    toggle("MotionBlur", "engine.graphics.setting.motion_blur", "Graphics"),
    toggle("SoftParticles", "engine.graphics.setting.soft_particles", "Graphics"),
    choice(
        "GlobalIllumination",
        "engine.graphics.setting.global_illumination",
        Enum.GraphicsLevel:GetEnumItems(),
        "engine.graphics.level."
    ),
    choice(
        "Reflections",
        "engine.graphics.setting.reflections",
        Enum.GraphicsLevel:GetEnumItems(),
        "engine.graphics.level."
    ),
    choice(
        "FogQuality",
        "engine.graphics.setting.fog_quality",
        Enum.GraphicsLevel:GetEnumItems(),
        "engine.graphics.level."
    ),
    {
        Setting = "WindowMode",
        Page = "Display",
        Caption = "engine.graphics.setting.window_mode",
        Kind = "Choice",
        Items = Enum.WindowMode:GetEnumItems(),
        ItemKey = "engine.graphics.choice.window_mode.",
        Held = true,
        Desktop = true,
    },
    {
        Setting = "Resolution",
        Page = "Display",
        Caption = "engine.graphics.setting.resolution_width",
        Kind = "Resolution",
        Held = true,
        Desktop = true,
    },
    {
        Setting = "Monitor",
        Page = "Display",
        Caption = "engine.graphics.setting.monitor",
        Kind = "Monitor",
        Held = true,
        Desktop = true,
    },
    toggle("VSync", "engine.graphics.setting.vsync", "Display", true),
    toggle("FrameGeneration", "engine.graphics.setting.frame_generation", "Display"),
    {
        Setting = "MaxFrameRate",
        Page = "Display",
        Caption = "engine.graphics.setting.max_frame_rate",
        Kind = "Steps",
        Steps = { 0, 30, 60, 120, 144, 240 },
        Format = "engine.settings.value.frame_rate",
        Zero = "engine.settings.value.unlimited",
    },
}

-- What is written in the order a window wants it: which display, how it sits
-- on it, and then how large.
local HeldOrder = { "Monitor", "WindowMode", "Resolution" }

-- A screen is laid out as if the window were this tall, whatever it is.
local ReferenceHeight = 720
local RowHeight = 44
local RowGap = 6
local StepWidth = 44

local function translate(key: string, arguments: { [string]: any }?): string
    return LocalizationService:Translate(key, arguments)
end

-- Two numbers a setting would call the same: what is written as 0.6 is read
-- back as the nearest thing a float holds.
local function same(a: number, b: number): boolean
    return math.abs(a - b) < 1e-4
end

local function isHandheld(): boolean
    local platform = RunService.Platform
    return platform == Enum.Platform.Android or platform == Enum.Platform.IOS
end

-- The sizes a player picks from: the project's own first, as zero, and then
-- what the display offers from the smallest up.
local function resolutions(): { Vector2 }
    local found = GraphicsService:GetSupportedResolutions()
    table.sort(found, function(a: Vector2, b: Vector2)
        if a.X ~= b.X then
            return a.X < b.X
        end
        return a.Y < b.Y
    end)
    table.insert(found, 1, Vector2.zero)
    return found
end

--- Opens the options screen and returns it. One at a time is what a game
--- wants; a second call makes a second screen over the first.
function Settings.open(options: Options?): Screen
    local theme: Theme = if options and options.Theme then options.Theme else {}
    local backdropColor = theme.Backdrop or Color3.fromRGB(8, 10, 16)
    local panel = theme.Panel or Color3.fromRGB(30, 34, 46)
    local raised = theme.Raised or Color3.fromRGB(44, 50, 68)
    local accent = theme.Accent or Color3.fromRGB(86, 156, 255)
    local ink = theme.Text or Color3.fromRGB(226, 230, 240)
    local dim = theme.Dim or Color3.fromRGB(150, 158, 178)
    local radius = theme.CornerRadius or 8

    local hidden: { [string]: boolean } = {}
    if options and options.Hide then
        for _, name in options.Hide do
            hidden[name] = true
        end
    end

    local handheld = isHandheld()
    local monitors = GraphicsService:GetMonitors()

    -- What the window settings will be at Apply, by setting.
    local held: { [string]: any } = {}
    local built: { Built } = {}
    local bySetting: { [string]: Built } = {}
    local connections: { Connection } = {}
    local selectedBefore = UIService.SelectedObject

    local gui = Instance.new("ScreenGui")
    gui.Name = "Settings"
    gui.ReferenceHeight = ReferenceHeight
    gui.DisplayOrder = if options and options.DisplayOrder then options.DisplayOrder else 100

    local function corner(parent: Instance, amount: number)
        local rounded = Instance.new("UICorner")
        rounded.CornerRadius = UDim.new(0, amount)
        rounded.Parent = parent
    end

    local function label(parent: Instance, size: number, color: Color3): TextLabel
        local made = Instance.new("TextLabel")
        made.BackgroundTransparency = 1
        made.Text = ""
        made.TextSize = size
        made.TextColor = color
        if theme.Font then
            made.Font = theme.Font
        end
        made.Parent = parent
        return made
    end

    -- **Words fit their box**: as large as `size`, and smaller where a
    -- language says the thing in more letters or the window is a phone's.
    local function fit(words: TextLabel | TextButton, size: number)
        words.TextScaled = true
        local bounds = Instance.new("UITextSizeConstraint")
        bounds.MinTextSize = 10
        bounds.MaxTextSize = size
        bounds.Parent = words
    end

    local function button(parent: Instance, size: number): TextButton
        local made = Instance.new("TextButton")
        made.Text = ""
        made.TextSize = size
        made.TextColor = ink
        made.BackgroundColor = raised
        if theme.Font then
            made.Font = theme.Font
        end
        corner(made, radius)
        made.Parent = parent
        return made
    end

    -- The game stays in sight behind, so a change is seen as it is made; the
    -- backdrop takes the pointer, so nothing under the menu is pressed.
    local backdrop = Instance.new("Frame")
    backdrop.Name = "Backdrop"
    backdrop.Size = UDim2.fromScale(1, 1)
    backdrop.BackgroundColor = backdropColor
    backdrop.BackgroundTransparency = theme.BackdropTransparency or 0.35
    backdrop.Parent = gui

    local window = Instance.new("Frame")
    window.Name = "Window"
    window.AnchorPoint = Vector2.new(0.5, 0.5)
    window.Position = UDim2.fromScale(0.5, 0.5)
    -- Nearly all of a phone held upright, and on a monitor no wider than a
    -- row is comfortable to read across.
    window.Size = UDim2.fromScale(0.94, 0.88)
    window.BackgroundColor = panel
    corner(window, radius + 6)
    window.Parent = backdrop

    local limits = Instance.new("UISizeConstraint")
    limits.MinSize = Vector2.new(300, 380)
    limits.MaxSize = Vector2.new(860, 660)
    limits.Parent = window

    local pad = Instance.new("UIPadding")
    pad.PaddingTop = UDim.new(0, 18)
    pad.PaddingBottom = UDim.new(0, 18)
    pad.PaddingLeft = UDim.new(0, 18)
    pad.PaddingRight = UDim.new(0, 18)
    pad.Parent = window

    local title = label(window, 26, ink)
    title.Name = "Title"
    title.Size = UDim2.new(1, 0, 0, 34)
    title.TextXAlignment = Enum.HorizontalAlignment.Left

    local tabs = Instance.new("Frame")
    tabs.Name = "Pages"
    tabs.Position = UDim2.fromOffset(0, 46)
    tabs.Size = UDim2.new(1, 0, 0, 42)
    tabs.BackgroundTransparency = 1
    tabs.Parent = window

    local tabRow = Instance.new("UIListLayout")
    tabRow.FillDirection = Enum.FillDirection.Horizontal
    tabRow.Padding = UDim.new(0, 10)
    tabRow.HorizontalFlex = Enum.UIFlexAlignment.Fill
    tabRow.ItemLineAlignment = Enum.ItemLineAlignment.Stretch
    tabRow.Parent = tabs

    -- What the heading and the pages' buttons take above, and the bar below.
    local Above = 46 + 42 + 12
    local Below = 46 + 12

    local body = Instance.new("Frame")
    body.Name = "Body"
    body.Position = UDim2.fromOffset(0, Above)
    body.Size = UDim2.new(1, 0, 1, -(Above + Below))
    body.BackgroundTransparency = 1
    body.ClipsDescendants = true
    body.Parent = window

    local bar = Instance.new("Frame")
    bar.Name = "Bar"
    bar.AnchorPoint = Vector2.new(0, 1)
    bar.Position = UDim2.fromScale(0, 1)
    bar.Size = UDim2.new(1, 0, 0, 46)
    bar.BackgroundTransparency = 1
    bar.Parent = window

    local barRow = Instance.new("UIListLayout")
    barRow.FillDirection = Enum.FillDirection.Horizontal
    barRow.Padding = UDim.new(0, 10)
    barRow.HorizontalFlex = Enum.UIFlexAlignment.Fill
    barRow.ItemLineAlignment = Enum.ItemLineAlignment.Stretch
    barRow.Parent = bar

    local pageNames: { Page } = { "Graphics", "Display" }
    local pageFrames: { [Page]: ScrollFrame } = {}
    local pageButtons: { [Page]: TextButton } = {}
    local pageCounts: { [Page]: number } = { Graphics = 0, Display = 0 }
    local currentPage: Page = "Graphics"

    for index, name in pageNames do
        local scroll = Instance.new("ScrollFrame")
        scroll.Name = name
        scroll.Size = UDim2.fromScale(1, 1)
        scroll.BackgroundTransparency = 1
        scroll.ScrollBarThickness = 8
        scroll.Visible = index == 1
        scroll.Parent = body

        local column = Instance.new("UIListLayout")
        column.Padding = UDim.new(0, RowGap)
        column.Parent = scroll

        pageFrames[name] = scroll

        local tab = button(tabs, 20)
        tab.Name = name
        tab.LayoutOrder = index
        -- A share of the bar and no width of its own: two pages fit a phone
        -- held upright as they fit a monitor.
        tab.Size = UDim2.new(0, 0, 1, 0)
        fit(tab, 20)
        pageButtons[name] = tab
    end

    local apply = button(bar, 20)
    apply.Name = "Apply"
    apply.LayoutOrder = 1
    apply.Size = UDim2.new(0, 0, 1, 0)
    fit(apply, 20)

    local revert = button(bar, 20)
    revert.Name = "Revert"
    revert.LayoutOrder = 2
    revert.Size = UDim2.new(0, 0, 1, 0)
    fit(revert, 20)

    local defaults = button(bar, 20)
    defaults.Name = "Defaults"
    defaults.LayoutOrder = 3
    defaults.Size = UDim2.new(0, 0, 1, 0)
    fit(defaults, 20)

    local back = button(bar, 20)
    back.Name = "Back"
    back.LayoutOrder = 4
    back.Size = UDim2.new(0, 0, 1, 0)
    fit(back, 20)

    local screen = {
        Gui = gui,
        IsOpen = true,
        IsDirty = false,
        Closed = Signal.new() :: Signal<>,
    } :: Screen

    -- A row's value: what is held for it, or what the service has.
    local function valueOf(row: Row): any
        local waiting = held[row.Setting]
        if waiting ~= nil then
            return waiting
        end
        return (GraphicsService :: any)[row.Setting]
    end

    local function wordsOf(row: Row): string
        local value = valueOf(row)
        if row.Kind == "Preset" then
            return translate("engine.graphics.quality." .. string.lower(value.Name))
        elseif row.Kind == "Toggle" then
            return translate(if value then "engine.settings.value.on" else "engine.settings.value.off")
        elseif row.Kind == "Choice" then
            return translate((row.ItemKey :: string) .. string.lower(value.Name))
        elseif row.Kind == "Steps" then
            if row.Zero and value == 0 then
                return translate(row.Zero)
            end
            local shown = math.round(value * (row.Scale or 1))
            return translate(row.Format :: string, { value = tostring(shown) })
        elseif row.Kind == "Resolution" then
            if value.X == 0 or value.Y == 0 then
                return translate("engine.settings.value.default")
            end
            return translate(
                "engine.settings.value.resolution",
                { width = tostring(value.X), height = tostring(value.Y) }
            )
        end
        local monitor = monitors[value + 1]
        return translate(
            "engine.settings.value.monitor",
            { number = tostring(value + 1), name = if monitor then monitor.Name else "" }
        )
    end

    local function refresh()
        if not screen.IsOpen then
            return
        end
        title.Text = if options and options.Title then options.Title else translate("engine.settings.title")
        pageButtons.Graphics.Text = translate("engine.settings.page.graphics")
        pageButtons.Display.Text = translate("engine.settings.page.display")
        apply.Text = translate("engine.settings.apply")
        revert.Text = translate("engine.settings.revert")
        defaults.Text = translate("engine.settings.defaults")
        back.Text = translate("engine.settings.back")
        apply.BackgroundColor = if screen.IsDirty then accent else raised
        for _, name in pageNames do
            pageButtons[name].BackgroundColor = if name == currentPage then accent else raised
        end
        for _, each in built do
            each.Caption.Text = translate(each.Row.Caption)
            each.Value.Text = wordsOf(each.Row)
            if each.Row.Kind == "Toggle" then
                each.Value.BackgroundColor = if valueOf(each.Row) then accent else raised
            end
        end
    end

    -- Writes a row's new value where it goes: held for Apply, or to the
    -- service now.
    local function write(row: Row, value: any)
        if row.Held then
            local standing = (GraphicsService :: any)[row.Setting]
            -- Stepped back to what the window already is: nothing to apply.
            held[row.Setting] = if value == standing then nil else value
        else
            (GraphicsService :: any)[row.Setting] = value
        end
        screen.IsDirty = true
    end

    -- The place of `wanted` in `stops`, `direction` along: the next stop above
    -- or below it, whether or not it is one of them.
    local function nextStop(stops: { number }, wanted: number, direction: number): number?
        if direction > 0 then
            for _, stop in stops do
                if stop > wanted and not same(stop, wanted) then
                    return stop
                end
            end
        else
            for index = #stops, 1, -1 do
                local stop = stops[index]
                if stop < wanted and not same(stop, wanted) then
                    return stop
                end
            end
        end
        return nil
    end

    local function step(row: Row, direction: number): boolean
        local value = valueOf(row)
        local toward = if direction < 0 then -1 else 1
        if row.Kind == "Toggle" then
            write(row, not value)
            return true
        elseif row.Kind == "Preset" then
            local at = table.find(Levels, value)
            if at == nil then
                -- From a mix of levels, a step either way goes to the
                -- machine's own.
                GraphicsService:ApplyPreset(Enum.GraphicsQuality.Auto)
                screen.IsDirty = true
                return true
            end
            local level = Levels[at + toward]
            if level == nil then
                return false
            end
            GraphicsService:ApplyPreset(level)
            screen.IsDirty = true
            return true
        elseif row.Kind == "Choice" then
            local items = row.Items :: { any }
            local at = table.find(items, value) or 1
            local item = items[at + toward]
            if item == nil then
                return false
            end
            write(row, item)
            return true
        elseif row.Kind == "Steps" then
            local stop = nextStop(row.Steps :: { number }, value, toward)
            if stop == nil then
                return false
            end
            write(row, stop)
            return true
        elseif row.Kind == "Resolution" then
            local sizes = resolutions()
            local at = 1
            for index, size in sizes do
                if size.X == value.X and size.Y == value.Y then
                    at = index
                    break
                end
            end
            local size = sizes[at + toward]
            if size == nil then
                return false
            end
            write(row, size)
            return true
        end
        local monitor = value + toward
        if monitor < 0 or monitor >= #monitors then
            return false
        end
        write(row, monitor)
        return true
    end

    -- Scrolls `row` into sight: a selection that walked off the bottom of the
    -- list is a selection the player cannot see.
    local function reveal(scroll: ScrollFrame, row: Frame)
        local unit = UIService.ViewportSize.Y / ReferenceHeight
        if unit <= 0 then
            return
        end
        local top = row.AbsolutePosition.Y - scroll.AbsolutePosition.Y
        local bottom = top + row.AbsoluteSize.Y - scroll.AbsoluteSize.Y
        local position = scroll.CanvasPosition
        if top < 0 then
            scroll.CanvasPosition = Vector2.new(position.X, position.Y + top / unit)
        elseif bottom > 0 then
            scroll.CanvasPosition = Vector2.new(position.X, position.Y + bottom / unit)
        end
    end

    local function stepped(row: Row, direction: number)
        if screen.IsOpen and step(row, direction) then
            refresh()
        end
    end

    for _, row in Rows do
        local shown = not hidden[row.Setting]
            and GraphicsService:IsApplied(row.Setting)
            and not (row.Desktop and handheld)
            -- One display is no choice of display.
            and not (row.Kind == "Monitor" and #monitors < 2)
        if not shown then
            continue
        end

        local scroll = pageFrames[row.Page]
        pageCounts[row.Page] += 1

        local frame = Instance.new("Frame")
        frame.Name = row.Setting
        frame.LayoutOrder = pageCounts[row.Page]
        -- Clear of the scroll bar.
        frame.Size = UDim2.new(1, -14, 0, RowHeight)
        frame.BackgroundTransparency = 1
        frame.Parent = scroll

        local caption = label(frame, 20, dim)
        caption.Name = "Caption"
        caption.Size = UDim2.new(0.5, -8, 1, 0)
        caption.TextXAlignment = Enum.HorizontalAlignment.Left
        fit(caption, 20)

        local value: TextLabel | TextButton
        local controls: { TextButton } = {}
        if row.Kind == "Toggle" then
            local switch = button(frame, 20)
            switch.Name = "Value"
            switch.Position = UDim2.fromScale(0.5, 0)
            switch.Size = UDim2.fromScale(0.5, 1)
            fit(switch, 20)
            table.insert(
                connections,
                switch.Activated:Connect(function()
                    stepped(row, 1)
                end)
            )
            table.insert(controls, switch)
            value = switch
        else
            local box = Instance.new("Frame")
            box.Name = "Box"
            box.Position = UDim2.fromScale(0.5, 0)
            box.Size = UDim2.fromScale(0.5, 1)
            box.BackgroundColor = raised
            box.BackgroundTransparency = 0.5
            corner(box, radius)
            box.Parent = frame

            local less = button(box, 22)
            less.Name = "Less"
            less.Text = "<"
            less.Size = UDim2.new(0, StepWidth, 1, 0)
            table.insert(
                connections,
                less.Activated:Connect(function()
                    stepped(row, -1)
                end)
            )

            local words = label(box, 20, ink)
            words.Name = "Value"
            words.Position = UDim2.fromOffset(StepWidth, 0)
            words.Size = UDim2.new(1, -StepWidth * 2, 1, 0)
            fit(words, 20)

            local more = button(box, 22)
            more.Name = "More"
            more.Text = ">"
            more.AnchorPoint = Vector2.new(1, 0)
            more.Position = UDim2.fromScale(1, 0)
            more.Size = UDim2.new(0, StepWidth, 1, 0)
            table.insert(
                connections,
                more.Activated:Connect(function()
                    stepped(row, 1)
                end)
            )

            table.insert(controls, less)
            table.insert(controls, more)
            value = words
        end
        for _, control in controls do
            table.insert(
                connections,
                control.SelectionGained:Connect(function()
                    reveal(scroll, frame)
                end)
            )
        end

        local made: Built = { Row = row, Frame = frame, Caption = caption, Value = value }
        table.insert(built, made)
        bySetting[row.Setting] = made
    end

    for _, name in pageNames do
        local count = pageCounts[name]
        pageFrames[name].CanvasSize = UDim2.fromOffset(0, count * RowHeight + math.max(count - 1, 0) * RowGap)
    end

    function screen.ShowPage(self: Screen, page: Page)
        if pageFrames[page] == nil then
            error(translate("engine.settings.err.no_page", { page = page }), 2)
        end
        currentPage = page
        for _, name in pageNames do
            pageFrames[name].Visible = name == page
        end
        refresh()
    end

    function screen.Step(self: Screen, setting: string, direction: number): boolean
        local row = bySetting[setting]
        if row == nil then
            error(translate("engine.settings.err.no_row", { setting = setting }), 2)
        end
        if not self.IsOpen then
            return false
        end
        local changed = step(row.Row, direction)
        if changed then
            refresh()
        end
        return changed
    end

    function screen.GetValueText(self: Screen, setting: string): string?
        local row = bySetting[setting]
        return if row then wordsOf(row.Row) else nil
    end

    function screen.Apply(self: Screen): boolean
        for _, setting in HeldOrder do
            local value = held[setting]
            if value ~= nil then
                (GraphicsService :: any)[setting] = value
            end
        end
        table.clear(held)
        local kept = GraphicsService:SaveAsync()
        self.IsDirty = false
        refresh()
        return kept
    end

    function screen.Revert(self: Screen)
        table.clear(held)
        GraphicsService:LoadAsync()
        self.IsDirty = false
        refresh()
    end

    function screen.ResetToDefaults(self: Screen)
        table.clear(held)
        GraphicsService:ResetToDefaults()
        self.IsDirty = true
        refresh()
    end

    function screen.Close(self: Screen)
        if not self.IsOpen then
            return
        end
        self.IsOpen = false
        for _, connection in connections do
            connection:Disconnect()
        end
        table.clear(connections)
        table.clear(held)
        if self.IsDirty then
            self.IsDirty = false
            -- On its own thread: `LoadAsync` yields, and closing a menu must
            -- not need a thread that can.
            task.spawn(function()
                GraphicsService:LoadAsync()
            end)
        end
        -- The selection goes back to what had it, so the menu under this one
        -- is where a gamepad still is.
        if selectedBefore ~= nil and selectedBefore.Parent ~= nil then
            UIService.SelectedObject = selectedBefore
        elseif UIService.SelectedObject ~= nil and UIService.SelectedObject:IsDescendantOf(gui) then
            UIService.SelectedObject = nil
        end
        gui:Destroy()
        self.Closed:Fire()
    end

    for _, name in pageNames do
        table.insert(
            connections,
            pageButtons[name].Activated:Connect(function()
                screen:ShowPage(name)
            end)
        )
    end
    table.insert(
        connections,
        apply.Activated:Connect(function()
            screen:Apply()
        end)
    )
    table.insert(
        connections,
        revert.Activated:Connect(function()
            screen:Revert()
        end)
    )
    table.insert(
        connections,
        defaults.Activated:Connect(function()
            screen:ResetToDefaults()
        end)
    )
    table.insert(
        connections,
        back.Activated:Connect(function()
            screen:Close()
        end)
    )
    -- Whoever else changes a setting, or the language, the rows say so.
    table.insert(connections, GraphicsService.QualityChanged:Connect(refresh))
    table.insert(connections, LocalizationService.LocaleChanged:Connect(refresh))

    refresh()
    gui.Parent = UIService
    -- A player on a gamepad had something selected: the menu takes the
    -- selection. A player on a mouse had nothing, and gets no outline.
    if selectedBefore ~= nil then
        UIService.SelectedObject = pageButtons.Graphics
    end
    return screen
end

return Settings
