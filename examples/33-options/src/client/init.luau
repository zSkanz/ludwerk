--!strict
--[[
	Options (ADR 0147, ADR 0154): a small scene with a button that opens the
	engine's own options screen, and a second that changes the language.

	What does each job:

	- `require("@engine/settings").open()` is the whole options menu: the
	  quality level, each setting this build draws by, the window, and Apply,
	  Revert, Defaults and Back. Step the level and watch the shadows and the
	  glow behind it change;
	- the two buttons' words come from this project's catalogs, `i18n/en.json`
	  and `i18n/pt-BR.json`, through `LocalizationService:Translate`;
	- `i18n/pt-BR.json` also says a few of the ENGINE's keys -- the screen's
	  heading and its four buttons -- which is how a game re-words the screen
	  or says it in a language the engine has no catalog for;
	- Escape, or Start on a gamepad, opens and closes the screen; every
	  control of it is a button, so the d-pad and the arrows walk it.
]]

local settings = require("@engine/settings")

local Lighting = game:GetService("Lighting")
local LocalizationService = game:GetService("LocalizationService")
local RunService = game:GetService("RunService")
local UIService = game:GetService("UIService")

-- --- The scene: things a quality level shows on -----------------------------------

Lighting.ClockTime = 15.5

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Size = Vector3.new(80, 1, 80)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(118, 124, 132))
floor.Anchored = true
floor.Parent = workspace

-- A ring of thin columns: what the shadow's resolution and reach show on.
for index = 1, 16 do
    local angle = index / 16 * math.pi * 2
    local column = Instance.new("Part")
    column.Name = `Column{index}`
    column.Size = Vector3.new(0.5, 3 + (index % 4) * 1.5, 0.5)
    column.CFrame = CFrame.new(math.cos(angle) * 9, column.Size.y / 2, math.sin(angle) * 9)
    column:SetMaterialParameter("Color", Color3.fromRGB(204, 96, 64))
    column.Anchored = true
    column.Parent = workspace
end

-- And lamps in the middle: what bloom shows on.
for index = 1, 3 do
    local lamp = Instance.new("Part")
    lamp.Name = `Lamp{index}`
    lamp.Shape = Enum.PartShape.Ball
    lamp.Size = Vector3.new(1.2, 1.2, 1.2)
    lamp.CFrame = CFrame.new((index - 2) * 3, 1.4, 0)
    lamp:SetMaterialParameter("Color", Color3.fromRGB(255, 240, 200))
    lamp:SetMaterialParameter("Emissive", Color3.new(10, 8, 5))
    lamp.Anchored = true
    lamp.Parent = workspace
end

local camera = Instance.new("Camera")
camera.FieldOfView = 55
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The camera goes round, slowly: a still picture hides what a shadow does
-- when it moves.
local turned = 0
RunService.Heartbeat:Connect(function(dt: number)
    turned += dt * 0.12
    local eye = Vector3.new(math.cos(turned) * 20, 8, math.sin(turned) * 20)
    camera.CFrame = CFrame.lookAt(eye, Vector3.new(0, 2, 0))
end)

-- --- The two buttons ---------------------------------------------------------------

local Raised = Color3.fromRGB(44, 50, 68)
local Text = Color3.fromRGB(226, 230, 240)

local hud = Instance.new("ScreenGui")
hud.Name = "Hud"
hud.ReferenceHeight = 720
hud.Parent = UIService

local function button(order: number): TextButton
    local made = Instance.new("TextButton")
    made.AnchorPoint = Vector2.new(1, 0)
    made.Position = UDim2.new(1, -20, 0, 20 + (order - 1) * 56)
    made.Size = UDim2.fromOffset(220, 46)
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
local languageButton = button(2)

local hint = Instance.new("TextLabel")
hint.AnchorPoint = Vector2.new(0.5, 1)
hint.Position = UDim2.new(0.5, 0, 1, -18)
hint.Size = UDim2.new(1, -40, 0, 24)
hint.BackgroundTransparency = 1
hint.TextSize = 16
hint.TextColor = Text
hint.Parent = hud

-- Every word on the screen, said again whenever the language changes.
local function caption()
    optionsButton.Text = LocalizationService:Translate("menu.options")
    languageButton.Text = LocalizationService:Translate("menu.language", { locale = LocalizationService.Locale })
    hint.Text = LocalizationService:Translate("menu.hint")
end
caption()
LocalizationService.LocaleChanged:Connect(caption)

-- --- The options screen ------------------------------------------------------------

local open: settings.Screen? = nil

local function toggle()
    if open then
        open:Close()
        return
    end
    local screen = settings.open()
    open = screen
    screen.Closed:Connect(function()
        open = nil
    end)
end

optionsButton.Activated:Connect(toggle)

languageButton.Activated:Connect(function()
    -- The next locale the project has a catalog for, going round.
    local locales = LocalizationService:GetLocales()
    local at = table.find(locales, LocalizationService.Locale) or 0
    LocalizationService.Locale = locales[at % #locales + 1]
end)

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
