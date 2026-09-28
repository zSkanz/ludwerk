--!strict
--[[
	The menu scene's own code (ADR 0105): a `Script` under this scene's
	`ClientScriptService`, from `src/scenes/menu/client/`. It exists only while
	the menu is loaded -- its screen and its key handler go with it.

	- Space plays alone: to the arena, with the round in the load's data,
	  which the arena reads with `GetLoadData`. The menu asks the game for it
	  (`game:SendMessage("GoTo", ...)`, ADR 0124), and the game's own code
	  prepares it behind a progress bar (`LoadSceneAsync`, ADR 0125).
	- H hosts a match (`NetworkService:Host`) and opens the lobby.
	- J joins one on this machine (`NetworkService:Join`): the server's scene --
	  its lobby -- replaces this one.
]]

local InputService = game:GetService("InputService")
local NetworkService = game:GetService("NetworkService")
local SceneService = game:GetService("SceneService")
local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Name = "Menu"
screen.Parent = UIService

local title = Instance.new("TextLabel")
title.Size = UDim2.new(1, 0, 0, 80)
title.Position = UDim2.new(0, 0, 0.35, 0)
title.Text = "SCENES"
title.TextSize = 48
title.TextColor = Color3.fromRGB(240, 236, 224)
title.BackgroundTransparency = 1
title.Parent = screen

local hint = Instance.new("TextLabel")
hint.Size = UDim2.new(1, 0, 0, 40)
hint.Position = UDim2.new(0, 0, 0.55, 0)
hint.Text = "Space to play alone -- H to host -- J to join"
hint.TextSize = 18
hint.TextColor = Color3.fromRGB(180, 188, 204)
hint.BackgroundTransparency = 1
hint.Parent = screen

-- The round the arena said it was, when the menu is reached from it.
local previous = SceneService:GetLoadData()
local round = 1
if typeof(previous) == "table" and typeof(previous.Round) == "number" then
    round = previous.Round + 1
end

NetworkService.JoinFailed:Connect(function(reason: string)
    hint.Text = reason
end)

InputService.InputBegan:Connect(function(input: InputObject, uiConsumed: boolean)
    if uiConsumed then
        return
    end
    if input.KeyCode == Enum.KeyCode.Space or input.KeyCode == Enum.KeyCode.Return then
        -- The game prepares the arena behind a loading bar (`Loading.luau`):
        -- asked for by message, since the menu goes with its scene.
        game:SendMessage("GoTo", "scenes/arena.scene.json", { Round = round })
    elseif input.KeyCode == Enum.KeyCode.H then
        NetworkService:Host()
        SceneService:LoadScene("scenes/lobby.scene.json")
    elseif input.KeyCode == Enum.KeyCode.J then
        hint.Text = "Joining..."
        NetworkService:Join("127.0.0.1")
    end
end)
