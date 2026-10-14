--!strict
--- Two first opens at once. A project nobody has opened is opened by
--- two processes in the same moment; this is the script each of them runs.
--- It stamps the walker, asks its model for a clip and says what it got: a
--- machine that read a store the other was half way through writing had a
--- model with no clips, and a `Length` of nought.
---
--- `run_two_first_opens.cmake` reads what each of the two wrote.

local RunService = game:GetService("RunService")

local hero = Instance.stamp("walker") :: CharacterBody
hero.Position = vector.create(0, 1.5, 0)
hero.Parent = workspace

local animator = hero:FindFirstChildOfClass("AnimationPlayer") :: AnimationPlayer
local ticks = 0
local said = false
RunService.PreSimulation:Connect(function()
    ticks += 1
    if said then
        return
    end
    local jump = animator:LoadAnimation("Jump" :: Content)
    local slash = animator:LoadAnimation("Slash" :: Content)
    if jump.Length > 0 and slash.Length > 0 then
        said = true
        print(`[firstopen] clips jump={string.format("%.2f", jump.Length)} slash={string.format("%.2f", slash.Length)}`)
    elseif ticks >= 90 then
        said = true
        print(`[firstopen] NO CLIPS after {ticks} ticks: jump={jump.Length} slash={slash.Length}`)
    end
end)
