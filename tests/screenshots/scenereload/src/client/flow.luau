--!strict

-- **The game's own code, which no scene takes with it**: it loads the field
-- again every half second, thirty times, and says which load it is on -- so
-- the driver can read, between those lines, what the process held.

local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

local step = 0
local tick = 0
print("SCENE field 0")
RunService.Heartbeat:Connect(function()
    tick += 1
    if tick % 30 ~= 0 or step >= 30 then
        return
    end
    step += 1
    print(`SCENE field {step}`)
    SceneService:LoadScene("scenes/field.scene.json")
end)
