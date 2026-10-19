--!strict
-- The player's machine: an account made, the refusals a sign-in can meet,
-- a sign-in, and a join with its ticket; then, across both worlds, what the
-- fields found on this machine's player.
local NetworkService = game:GetService("NetworkService")
local SceneService = game:GetService("SceneService")

type Load = { door: string? }
local Door = ((SceneService:GetLoadData() or {}) :: Load).door
local Password = "correct horse battery"

if Door ~= nil then
    task.spawn(function()
        local made = NetworkService:RegisterAsync(Door, "Ana_1", Password)
        print(`[accounts] registered ok={made.Ok}`)
        -- One that only looks like it, and the refusals.
        local again = NetworkService:RegisterAsync(Door, "ana_l", Password)
        print(`[accounts] again reason={again.Reason}`)
        local wrong = NetworkService:SignInAsync(Door, "Ana_1", "not the password")
        print(`[accounts] wrong reason={wrong.Reason}`)
        local nobody = NetworkService:SignInAsync(Door, "nobody", Password)
        print(`[accounts] nobody reason={nobody.Reason}`)
        local short = NetworkService:RegisterAsync(Door, "bo_2", "short")
        print(`[accounts] short reason={short.Reason}`)
        -- Signed in whatever the case it is typed in.
        local signedIn = NetworkService:SignInAsync(Door, "ana_1", Password)
        print(`[accounts] signed in ok={signedIn.Ok}`)
        NetworkService:Join(Door, { Ticket = signedIn.Ticket })
    end)
    task.spawn(function()
        while true do
            local me = NetworkService.LocalPlayer
            local level = if me ~= nil then me:GetAttribute("Level") else nil
            if me ~= nil and level ~= nil then
                print(`[accounts] client account={me:GetAttribute("Account")} level={level}`)
                return
            end
            task.wait()
        end
    end)
end
