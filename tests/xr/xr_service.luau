--!strict
-- XRService under the simulated headset (ADR 0245, ADR 0248 §4), and flat.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 10 then
        local head = XRService:GetHeadCFrame()
        print(
            `[xr] enabled={XRService.Enabled} head={string.format("%.2f", head.Position.Y)} `
                .. `left={XRService:IsHandTracked(Enum.XRHand.Left)}`
        )
    end
end)
