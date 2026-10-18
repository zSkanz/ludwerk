--!strict
-- The second session (ADR 0247 §3): the anchor the first saved, loaded from
-- the simulated headset, at the same place; and one destroyed, let go of.
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
    local anchors = XRService:LoadAnchorsAsync()
    RunService.PostSimulation:Wait()
    RunService.PostSimulation:Wait()
    local anchor = anchors[1]
    assert(anchor ~= nil, "the saved anchor is found")
    local look = anchor.CFrame.LookVector
    -- Loaded again: the same instance, not a second one.
    local again = XRService:LoadAnchorsAsync()
    print(
        `[anchor] loaded={#anchors} id={anchor.Id} tracked={anchor.Tracked} at={fixed(anchor.CFrame.Position.x)},`
            .. `{fixed(anchor.CFrame.Position.y)},{fixed(anchor.CFrame.Position.z)} look={fixed(look.x)},{fixed(look.z)} `
            .. `same={again[1] == anchor}`
    )
end)
