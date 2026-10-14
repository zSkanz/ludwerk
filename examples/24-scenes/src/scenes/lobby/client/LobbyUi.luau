--!strict
--[[
	The lobby's screen (ADR 0106): every player and whether they are ready,
	redrawn when a `Ready` attribute changes. R says ready.

	The key is an `InputAction`, so what this player presses reaches the
	server as intent; the server decides, and its attribute is what this list
	shows.
]]

local NetworkService = game:GetService("NetworkService")
local UIService = game:GetService("UIService")

local controls = Instance.new("InputContext")
controls.Name = "LobbyControls"
local ready = Instance.new("InputAction")
ready.Name = "Ready"
ready.Type = Enum.InputActionType.Bool
local key = Instance.new("InputBinding")
key.KeyCode = Enum.KeyCode.R
key.Parent = ready
ready.Parent = controls
controls.Parent = workspace

local screen = Instance.new("ScreenGui")
screen.Name = "Lobby"
screen.Parent = UIService

local list = Instance.new("TextLabel")
list.Size = UDim2.new(1, 0, 1, 0)
list.TextSize = 20
list.TextColor = Color3.fromRGB(232, 236, 244)
list.BackgroundTransparency = 1
list.TextWrapped = true
list.Parent = screen

local function redraw()
    local lines = { "LOBBY -- R when you are ready", "" }
    for _, player in NetworkService:GetPlayers() do
        local state = if player:GetAttribute("Ready") == true then "ready" else "waiting"
        table.insert(lines, `{player.Name}: {state}`)
    end
    list.Text = table.concat(lines, "\n")
end

local function watch(player: Player)
    player:GetAttributeChangedSignal("Ready"):Connect(redraw)
    redraw()
end

for _, player in NetworkService:GetPlayers() do
    watch(player)
end
NetworkService.PlayerAdded:Connect(watch)
NetworkService.PlayerRemoving:Connect(function()
    redraw()
end)
redraw()
