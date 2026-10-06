--!strict

-- **A picture drawn small in the interface is drawn from its smaller levels**
-- (D578). The picture is the finest thing a picture can hold -- a checkerboard
-- of single texels, 256 across, black and white -- shown at the four sizes a
-- game's icons are: 17, 22, 28 and 44 pixels. Drawn properly small it is its
-- own mean, an even grey half as bright as white. With the top level alone on
-- the GPU each pixel was whichever texels its centre fell among: black here,
-- white there, a pattern that is in no picture.

local UIService = game:GetService("UIService")

local screen = Instance.new("ScreenGui")
screen.Parent = UIService

local back = Instance.new("Frame")
back.Size = UDim2.fromScale(1, 1)
back.BackgroundColor = Color3.fromRGB(30, 30, 30)
back.Parent = screen

local function icon(x: number, size: number)
    local picture = Instance.new("ImageLabel")
    picture.Position = UDim2.fromOffset(x, 100)
    picture.Size = UDim2.fromOffset(size, size)
    picture.BackgroundTransparency = 1
    picture.Image = "asset://textures/checker256.png"
    picture.Parent = back
end

-- `run_ui_minify_gate.cmake` probes these places.
icon(100, 17)
icon(200, 22)
icon(300, 28)
icon(400, 44)
