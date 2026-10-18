--!strict
-- The first session (ADR 0247 §3): an anchor pinned half a metre to the
-- right, a metre ahead and a metre up, turned a quarter to the left, then
-- saved on the simulated headset.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local function fixed(value: number): string
    local text = string.format("%.2f", value)
    return if text == "-0.00" then "0.00" else text
end

task.spawn(function()
    while not XRService.Enabled do
        RunService.PostSimulation:Wait()
    end
    local pinned = CFrame.new(0.5, 1, -1) * CFrame.fromAxisAngle(vector.create(0, 1, 0), math.pi / 2)
    local anchor = XRService:CreateAnchorAsync(pinned)
    assert(anchor ~= nil, "the simulated headset has anchors")
    RunService.PostSimulation:Wait()
    RunService.PostSimulation:Wait()
    local look = anchor.CFrame.LookVector
    local saved = anchor:SaveAsync()
    print(
        `[anchor] saved={saved} id={anchor.Id} tracked={anchor.Tracked} at={fixed(anchor.CFrame.Position.x)},`
            .. `{fixed(anchor.CFrame.Position.y)},{fixed(anchor.CFrame.Position.z)} look={fixed(look.x)},{fixed(look.z)}`
    )
end)
