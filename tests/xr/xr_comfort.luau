--!strict
-- `@engine/xrcomfort` under the simulated headset (ADR 0246 §3): a teleport
-- lands on the floor where the arc meets it and never on a wall, the view
-- fades as it moves, and a snap turn turns the play space by 30 degrees about
-- the head each time the stick is pushed over.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")
local comfort = require("@engine/xrcomfort")

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(40, 1, 40)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = workspace

-- A wall to the player's left, three metres tall.
local wall = Instance.new("Part")
wall.Anchored = true
wall.Size = vector.create(0.2, 3, 10)
wall.Position = vector.create(-2, 1.5, 0)
wall.Parent = workspace

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 0)
camera.Parent = workspace
workspace.CurrentCamera = camera

local session = comfort.start({ Move = "teleport", Turn = "snap", SnapDegrees = 30 })

local function fixed(value: number): string
    local text = string.format("%.2f", value)
    return if text == "-0.00" then "0.00" else text
end

local function yaw(): number
    local look = camera.CFrame.LookVector
    return math.deg(math.atan2(-look.x, -look.z))
end

local ticks = 0
local landed = "?"
local faded = "?"
local walled = "?"
local turned: { string } = {}
RunService.Heartbeat:Connect(function(dt: number)
    session:Update(dt)
    ticks += 1
    if ticks == 5 then
        -- The left hand low and ahead, pointing down at the floor three
        -- metres ahead.
        XRService:SimulatePose(
            Enum.XRPose.LeftHand,
            CFrame.lookAt(vector.create(-0.2, 1.2, -0.3), vector.create(-0.2, 0, -3))
        )
    elseif ticks == 8 then
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickLeftY, 1)
    elseif ticks == 12 then
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickLeftY, 0)
    elseif ticks == 14 then
        faded = fixed(session.VignetteAmount)
    elseif ticks == 20 then
        landed = `{fixed(camera.CFrame.Position.x)},{fixed(camera.CFrame.Position.z)}`
        -- Now at the wall, level: it is not to stand on.
        XRService:SimulatePose(
            Enum.XRPose.LeftHand,
            CFrame.lookAt(vector.create(-0.2, 1.2, -2.3), vector.create(-3, 1.2, -2.3))
        )
    elseif ticks == 23 then
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickLeftY, 1)
    elseif ticks == 27 then
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickLeftY, 0)
    elseif ticks == 32 then
        walled = `{fixed(camera.CFrame.Position.x)},{fixed(camera.CFrame.Position.z)}`
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickRightX, 1)
    elseif ticks == 36 then
        table.insert(turned, string.format("%.0f", yaw()))
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickRightX, 0)
    elseif ticks == 40 then
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickRightX, 1)
    elseif ticks == 44 then
        table.insert(turned, string.format("%.0f", yaw()))
        XRService:SimulateInput(Enum.KeyCode.XRThumbstickRightX, 0)
    elseif ticks == 48 then
        print(
            `[comfort] landed={landed} faded={faded} walled={walled} turned={table.concat(turned, ",")} `
                .. `rest={fixed(session.VignetteAmount)}`
        )
    end
end)
