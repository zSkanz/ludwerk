--!strict
--[[
	Signing in (ADR 0237): the realms' servers let in only the players they
	signed in (`[accounts]` in `servers.toml`), so a player's machine started
	with the game's front door -- `run.bat play` -- shows this first.

	- **Sign in** and **Make an account** call `NetworkService:SignInAsync` and
	  `RegisterAsync`, which never raise for a refusal: the result says why,
	  and this says it in the player's words.
	- The ticket they return is good for one join, so it is used at once:
	  `NetworkService:Join(door, { Ticket = ticket })`.
	- The login name is the account's; what the realms call the player on
	  screen would be a display name of the game's own.
]]

local LocalizationService = game:GetService("LocalizationService")
local NetworkService = game:GetService("NetworkService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

type Load = { door: string? }
local Door = ((SceneService:GetLoadData() or {}) :: Load).door

if Door == nil then
    return
end
local door = Door :: string

local screen = Instance.new("ScreenGui")
screen.Name = "RealmLogin"
screen.KeepOnSceneLoad = true
screen.Parent = UIService

local panel = Instance.new("Frame")
panel.AnchorPoint = Vector2.new(0.5, 0.5)
panel.Position = UDim2.new(0.5, 0, 0.5, 0)
panel.Size = UDim2.new(0, 360, 0, 260)
panel.BackgroundColor = Color3.fromRGB(32, 30, 40)
panel.Parent = screen

local function label(text: string, y: number, size: number): TextLabel
    local made = Instance.new("TextLabel")
    made.Position = UDim2.new(0, 20, 0, y)
    made.Size = UDim2.new(1, -40, 0, 28)
    made.TextSize = size
    made.TextColor = Color3.fromRGB(240, 236, 226)
    made.BackgroundTransparency = 1
    made.Text = text
    made.Parent = panel
    return made
end

local function field(placeholder: string, y: number, masked: boolean): TextInput
    local made = Instance.new("TextInput")
    made.Position = UDim2.new(0, 20, 0, y)
    made.Size = UDim2.new(1, -40, 0, 34)
    made.TextSize = 18
    made.PlaceholderText = placeholder
    made.Masked = masked
    made.MaxLength = if masked then 128 else 20
    made.Parent = panel
    return made
end

local function button(text: string, x: number): TextButton
    local made = Instance.new("TextButton")
    made.Position = UDim2.new(0, x, 0, 170)
    made.Size = UDim2.new(0, 150, 0, 36)
    made.TextSize = 18
    made.Text = text
    made.Parent = panel
    return made
end

label(LocalizationService:Translate("realm.login.title"), 14, 24)
local name = field(LocalizationService:Translate("realm.login.name"), 56, false)
local password = field(LocalizationService:Translate("realm.login.password"), 100, true)
local signIn = button(LocalizationService:Translate("realm.login.sign_in"), 20)
local register = button(LocalizationService:Translate("realm.login.register"), 190)
local status = label("", 216, 16)
status.TextColor = Color3.fromRGB(250, 214, 120)

local busy = false

local function go(registering: boolean)
    if busy then
        return
    end
    busy = true
    status.Text = LocalizationService:Translate("realm.login.asking")
    local result = if registering
        then NetworkService:RegisterAsync(door, name.Text, password.Text)
        else NetworkService:SignInAsync(door, name.Text, password.Text)
    if result.Ok then
        password.Text = ""
        status.Text = LocalizationService:Translate("realm.login.joining")
        NetworkService:Join(door, { Ticket = result.Ticket })
        screen.Enabled = false
    else
        -- `realm.login.failure.WrongNameOrPassword` and the rest, by the
        -- reason's name.
        local reason = result.Reason
        status.Text = if reason then LocalizationService:Translate(`realm.login.failure.{reason.Name}`) else ""
    end
    busy = false
end

signIn.Activated:Connect(function()
    go(false)
end)
register.Activated:Connect(function()
    go(true)
end)
-- Back to this when the servers let the player go.
NetworkService.Disconnected:Connect(function()
    screen.Enabled = true
    status.Text = ""
end)
