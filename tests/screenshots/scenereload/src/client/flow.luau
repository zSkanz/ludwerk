--!strict

-- **The game's own code, which no scene takes with it**: it loads the field
-- again every half second, thirty times, and says at each load what the
-- process holds then -- so the driver reads it at the same moments every run,
-- whatever a frame takes.

local DebugService = game:GetService("DebugService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

local function held(): string
    return string.format("%.1f", DebugService:GetStat("ProcessMemoryMB"))
end

local step = 0
local tick = 0
print(`SCENE field 0 {held()} MiB`)
RunService.Heartbeat:Connect(function()
    tick += 1
    if tick % 30 ~= 0 or step > 30 then
        return
    end
    step += 1
    -- Half a second after the last load: what it left.
    if step > 30 then
        print(`SCENE field end {held()} MiB`)
        return
    end
    print(`SCENE field {step} {held()} MiB`)
    SceneService:LoadScene("scenes/field.scene.json")
end)
