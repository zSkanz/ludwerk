--!strict
-- Three markers in front of a simulated headset (`xr_stereo`): red one meter
-- straight ahead, green up and to the left, yellow down and to the right
-- and further. The head is turned and tilted, and each marker must land on
-- the same row of both eyes' pictures.
local XRService = game:GetService("XRService")

local function marker(position: vector, color: Color3)
    local made = Instance.new("Part")
    made.Anchored = true
    made.Shape = Enum.PartShape.Ball
    made.Size = vector.create(0.06, 0.06, 0.06)
    made.Position = position
    made:SetMaterialParameter("Color", color)
    made.Parent = workspace
end

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new()
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The head, turned 20 degrees and tilted down 10, and where it looks.
local Head = CFrame.new(0, 1.6, 0) * CFrame.fromEuler(math.rad(-10), math.rad(20), 0)
marker((Head * CFrame.new(0, 0, -1)).Position, Color3.fromRGB(255, 0, 0))
marker((Head * CFrame.new(-0.3, 0.25, -1)).Position, Color3.fromRGB(0, 255, 0))
marker((Head * CFrame.new(0.25, -0.3, -1.6)).Position, Color3.fromRGB(255, 255, 0))

task.spawn(function()
    while not XRService.Enabled do
        task.wait()
    end
    XRService:SimulatePose(Enum.XRPose.Head, Head)
end)
