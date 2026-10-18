--!strict
-- XRService under the simulated headset (ADR 0245, ADR 0248 §4), and flat.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local function fixed(value: number): string
    return string.format("%.2f", value)
end

local headMoves = 0
local states: { string } = {}
XRService.UserCFrameChanged:Connect(function(pose: EnumXRPose, _cframe: CFrame)
    if pose == Enum.XRPose.Head then
        headMoves += 1
    end
end)
XRService.SessionStateChanged:Connect(function(state: EnumXRSessionState)
    table.insert(states, state.Name)
end)

local ticks = 0
local simulated = "?"
local turned = "?"
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 10 then
        local head = XRService:GetHeadCFrame()
        print(
            `[xr] enabled={XRService.Enabled} head={fixed(head.Position.y)} `
                .. `left={XRService:IsHandTracked(Enum.XRHand.Left)}`
        )
        -- The head a metre aside and two back, turned a quarter to its left;
        -- the left hand held out.
        local ok = pcall(function()
            XRService:SimulatePose(
                Enum.XRPose.Head,
                CFrame.new(1, 1.7, 2) * CFrame.fromAxisAngle(vector.create(0, 1, 0), math.pi / 2)
            )
            XRService:SimulatePose(Enum.XRPose.LeftHand, CFrame.new(0.3, 1.2, -0.4))
        end)
        simulated = tostring(ok)
    elseif ticks == 20 then
        local head = XRService:GetHeadCFrame()
        local left = XRService:GetHandCFrame(Enum.XRHand.Left)
        local area = XRService:GetPlayAreaBounds()
        print(
            `[xr] simulated={simulated} head={fixed(head.Position.x)},{fixed(head.Position.y)},{fixed(head.Position.z)} `
                .. `left={XRService:IsHandTracked(Enum.XRHand.Left)} {fixed(left.Position.y)} `
                .. `area={if area then `{fixed(area.x)}x{fixed(area.z)}` else "nil"} `
                .. `state={XRService.SessionState.Name}`
        )
        XRService:Recenter()
    elseif ticks == 30 then
        -- Recentred: the head is the origin across the floor, facing ahead.
        local head = XRService:GetHeadCFrame()
        turned = `{fixed(head.Position.x)},{fixed(head.Position.y)},{fixed(head.Position.z)} `
            .. `look={fixed(head.LookVector.x)},{fixed(head.LookVector.z)}`
        XRService.Space = Enum.XRSpace.Seated
    elseif ticks == 40 then
        -- Seated: from the head at rest, and the recentre was the standing
        -- space's.
        local head = XRService:GetHeadCFrame()
        print(
            `[xr] recentred={turned} seated={fixed(head.Position.y)} space={XRService.Space.Name} `
                .. `moves={headMoves > 0} states={table.concat(states, ",")} `
                .. `joints={XRService:GetHandJoints(Enum.XRHand.Left) == nil}`
        )
    end
end)
