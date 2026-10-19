--!strict
-- The room seen through a headset (ADR 0247 §1): with `XRService.Passthrough`
-- on, the eyes' pictures are clear where the game draws nothing -- the sky --
-- and opaque where it draws the floor. A sign over the horizon, which writes
-- no depth, covers the room as much as it is opaque (ADR 0250), and a part
-- that shows the room is clear where it stands on the floor. Run under the
-- simulated headset with a GPU and `--screenshot`; `imgprobe` reads the
-- alpha of the left eye.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(40, 1, 40)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = workspace

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 0)
camera.Parent = workspace
workspace.CurrentCamera = camera

-- A sign in the sky ahead: nothing behind it but the room.
local post = Instance.new("Part")
post.Anchored = true
post.Size = vector.create(0.1, 0.1, 0.1)
post.Position = vector.create(0, 2.6, -5)
post.Fade = 1
post.Parent = workspace
local sign = Instance.new("BillboardGui")
sign.Size = UDim2.new(2, 0, 1, 0)
sign.Adornee = post
local face = Instance.new("Frame")
face.Size = UDim2.fromScale(1, 1)
face.BackgroundColor = Color3.fromRGB(200, 60, 60)
face.Parent = sign
sign.Parent = post

-- A window onto the room, standing on the floor ahead.
local window = Instance.new("Part")
window.Anchored = true
window.Size = vector.create(3, 1.2, 0.1)
window.Position = vector.create(0, 0.6, -3)
window.ShowsRoom = true
window.Parent = workspace

XRService.Passthrough = true

local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 10 then
        print(`[passthrough] supported={XRService:SupportsPassthrough()} on={XRService.Passthrough}`)
    end
end)
