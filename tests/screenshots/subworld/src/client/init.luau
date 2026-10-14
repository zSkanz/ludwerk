--!strict
-- `subworld_gate` (ADR 0107 §3): two sub-worlds running one scene, each drawn
-- into its own half of the screen. The right one was sent "blue"; the left one
-- was not. And the host's own world -- a red wall where the sub-worlds'
-- cameras look -- is in neither picture.

local UIService = game:GetService("UIService")

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 10)
camera.Parent = workspace
workspace.CurrentCamera = camera

local wall = Instance.new("Part")
wall.Anchored = true
wall.Size = vector.create(60, 60, 1)
wall.CFrame = CFrame.new(0, 0, -2)
wall:SetMaterialParameter("Color", Color3.fromRGB(220, 0, 0))
wall.Parent = workspace

local hud = Instance.new("ScreenGui")
hud.Parent = UIService

local function screen(name: string, x: number, message: string?)
    local world = Instance.new("SubWorld")
    world.Scene = "scenes/wall.scene.json"
    world.ViewName = name
    world.Resolution = Vector2.new(256, 256)
    world.Parent = workspace
    world:Load()
    if message then
        world:Send(message)
    end

    local picture = Instance.new("ImageLabel")
    picture.Position = UDim2.fromScale(x, 0)
    picture.Size = UDim2.fromScale(0.5, 1)
    picture.Image = `view://{name}`
    picture.Parent = hud
end

screen("left", 0, nil)
screen("right", 0.5, "blue")
