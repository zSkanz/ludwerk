--!strict
-- The player's machine, across both worlds: it says the level the fields
-- found, once it is on this machine's player.
local NetworkService = game:GetService("NetworkService")

task.spawn(function()
    while true do
        local me = NetworkService.LocalPlayer
        local level = if me ~= nil then me:GetAttribute("Level") else nil
        if level ~= nil then
            print(`[travel] client level={level}`)
            return
        end
        task.wait()
    end
end)
