--!strict
--[[
	The game's own code, for every scene (ADR 0105): a `Script` under
	`GlobalScriptService.Client`, from `src/client/`. It is never unloaded, so
	it is what is still here while one scene gives way to the next.

	It keeps a loading screen. `KeepOnSceneLoad` takes the screen with the game
	rather than with the scene, so it can go up on `SceneLoading`, stay while
	the menu is torn down and the arena read, and come down on `SceneLoaded`.
]]

local NetworkService = game:GetService("NetworkService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Name = "Loading"
screen.KeepOnSceneLoad = true
screen.DisplayOrder = 100
screen.Enabled = false
screen.Parent = UIService

local curtain = Instance.new("Frame")
curtain.Size = UDim2.new(1, 0, 1, 0)
curtain.BackgroundColor = Color3.fromRGB(10, 12, 18)
curtain.Parent = screen

local label = Instance.new("TextLabel")
label.Size = UDim2.new(1, 0, 1, 0)
label.Text = "Loading..."
label.TextSize = 28
label.TextColor = Color3.fromRGB(220, 224, 236)
label.BackgroundTransparency = 1
label.Parent = curtain

local visits = 0

SceneService.SceneLoading:Connect(function(path: string)
    label.Text = `Loading {path}...`
    screen.Enabled = true
end)

SceneService.SceneLoaded:Connect(function(path: string)
    visits += 1
    screen.Enabled = false
    -- A variable of the game's, kept across every change.
    print(`scenes: now in {path}, change {visits}`)
end)

-- **Out of a match, back to the menu**: the connection ended -- this machine
-- left, or the server went -- and this machine decides its world again.
NetworkService.Disconnected:Connect(function()
    if SceneService.CurrentScene ~= "scenes/menu.scene.json" then
        SceneService:LoadScene("scenes/menu.scene.json")
    end
end)
