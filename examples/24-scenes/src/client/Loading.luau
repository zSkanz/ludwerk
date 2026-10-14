--!strict
--[[
	The game's own code, for every scene (ADR 0105): a `Script` under
	`GlobalScriptService.Client`, from `src/client/`. It is never unloaded, so
	it is what is still here while one scene gives way to the next.

	It keeps a loading screen. `KeepOnSceneLoad` takes the screen with the game
	rather than with the scene, so it can go up on `SceneLoading`, stay while
	the menu is torn down and the arena read, and come down on `SceneLoaded`.

	**A scene asks for the next one by message** (ADR 0124): the menu sends
	`game:SendMessage("GoTo", path, data)`, and this script -- the game's --
	prepares that scene in the background with `LoadSceneAsync` (ADR 0125),
	fills a bar with its `Progress`, fades to black when it is `Ready`, and
	only then `Activate`s it.
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

local track = Instance.new("Frame")
track.AnchorPoint = Vector2.new(0.5, 0)
track.Position = UDim2.new(0.5, 0, 0.6, 0)
track.Size = UDim2.new(0.4, 0, 0, 6)
track.BackgroundColor = Color3.fromRGB(40, 44, 56)
track.Parent = curtain

local bar = Instance.new("Frame")
bar.Size = UDim2.new(0, 0, 1, 0)
bar.BackgroundColor = Color3.fromRGB(120, 180, 255)
bar.Parent = track

local visits = 0

-- Up for any change, whoever asked for it: a plain `LoadScene` from the lobby
-- or the arena comes through here too.
SceneService.SceneLoading:Connect(function(path: string)
    label.Text = `Loading {path}...`
    curtain.BackgroundTransparency = 0
    screen.Enabled = true
end)

SceneService.SceneLoaded:Connect(function(path: string)
    visits += 1
    screen.Enabled = false
    bar.Size = UDim2.new(0, 0, 1, 0)
    -- A variable of the game's, kept across every change.
    print(`scenes: now in {path}, change {visits}`)
end)

-- **The next scene, prepared while this one plays.** The bar shows how much of
-- it is in; the switch waits for a short fade, which the game decides.
game:BindToMessage("GoTo", function(path: string, data: any)
    local loading = SceneService:LoadSceneAsync(path, { Activate = false, Data = data })
    label.Text = `Preparing {path}...`
    curtain.BackgroundTransparency = 1
    screen.Enabled = true
    while loading.Status == Enum.SceneLoadStatus.Preparing do
        bar.Size = UDim2.new(loading.Progress, 0, 1, 0)
        task.wait()
    end
    if loading.Status ~= Enum.SceneLoadStatus.Ready then
        screen.Enabled = false
        print(`scenes: {path} did not load: {loading.Error}`)
        return
    end
    bar.Size = UDim2.new(1, 0, 1, 0)
    for step = 1, 10 do
        curtain.BackgroundTransparency = 1 - step / 10
        task.wait()
    end
    loading:Activate()
end)

-- **Out of a match, back to the menu**: the connection ended -- this machine
-- left, or the server went -- and this machine decides its world again.
NetworkService.Disconnected:Connect(function()
    if SceneService.CurrentScene.Path ~= "scenes/menu.scene.json" then
        SceneService:LoadScene("scenes/menu.scene.json")
    end
end)
