--!strict
--[[
	The probe scene for camera textures (ADR 0107).

	Three walls a long way apart, each filling the view of one camera: green in
	front of the world's own camera, red and blue in front of two cameras whose
	`CameraTexture`s draw into `view://red` and `view://blue`. Two `ImageLabel`s
	show those names. `run_views_gate.cmake` asks `imgprobe` what colour each
	place is: the world's view is green, the left picture red, the right blue --
	two views drawn in one frame, each from its own camera. Rendered at 1000 by
	500.
]]

local UIService = game:GetService("UIService")

local function wall(x: number, color: Color3)
    local part = Instance.new("Part")
    part.Anchored = true
    part.Size = vector.create(60, 40, 1)
    part.CFrame = CFrame.new(x, 20, -30)
    part:SetMaterialParameter("Color", color)
    part.Parent = workspace
end

local function camera(name: string, x: number): Camera
    local lens = Instance.new("Camera")
    lens.Name = name
    lens.CFrame = CFrame.lookAt(vector.create(x, 20, 0), vector.create(x, 20, -30))
    lens.Parent = workspace
    return lens
end

wall(0, Color3.new(0, 1, 0))
wall(200, Color3.new(1, 0, 0))
wall(400, Color3.new(0, 0, 1))

workspace.CurrentCamera = camera("Main", 0)

local function feed(name: string, x: number)
    local texture = Instance.new("CameraTexture")
    texture.Name = name .. "Feed"
    texture.Camera = camera(name .. "Camera", x)
    texture.ViewName = name
    texture.Resolution = Vector2.new(256, 144)
    texture.Parent = workspace
end

feed("red", 200)
feed("blue", 400)

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local function monitor(name: string, x: number)
    local picture = Instance.new("ImageLabel")
    picture.Position = UDim2.fromOffset(x, 100)
    picture.Size = UDim2.fromOffset(320, 180)
    picture.Image = "view://" .. name
    picture.Parent = screen
end

-- x 60..380 and 620..940, y 100..280.
monitor("red", 60)
monitor("blue", 620)
