--!strict

-- **The game's own code, which no scene takes with it** (D610): it goes
-- lobby > a > lobby > b > lobby, two seconds a scene, and says where it is --
-- so the driver can read, between those lines, what the graphics card held.

local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

local Order = { "a", "lobby", "b", "lobby" }
local step = 0
local tick = 0

print("SCENE lobby 0")
RunService.Heartbeat:Connect(function()
    tick += 1
    if tick % 120 ~= 0 then
        return
    end
    step += 1
    if step <= #Order then
        print(`SCENE {Order[step]} {step}`)
        SceneService:LoadScene(`scenes/{Order[step]}.scene.json`)
    elseif step == #Order + 1 then
        print("SCENE end")
    end
end)
