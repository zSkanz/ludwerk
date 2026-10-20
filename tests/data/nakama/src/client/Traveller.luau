--!strict
-- The player's machine: signed in to the game's Nakama with a device's id, the
-- session turned into a ticket at the front door, and a join with it; then,
-- across both worlds, the account and level the fields found on this player.
local NakamaService = game:GetService("NakamaService")
local NetworkService = game:GetService("NetworkService")
local SceneService = game:GetService("SceneService")

type Load = { door: string?, device: string? }
local load = (SceneService:GetLoadData() or {}) :: Load

if load.door ~= nil and load.device ~= nil then
    local door = load.door :: string
    task.spawn(function()
        local signed, why = NakamaService:AuthenticateAsync("device", load.device :: string)
        print(`[nakama] signed in to nakama={signed} {why or ""} session={#NakamaService.Session > 0}`)
        local result = NakamaService:GetTicketAsync(door)
        print(`[nakama] ticket ok={result.Ok}`)
        if result.Ok then
            NetworkService:Join(door, { Ticket = result.Ticket })
        end
    end)
    task.spawn(function()
        while true do
            local me = NetworkService.LocalPlayer
            local level = if me ~= nil then me:GetAttribute("Level") else nil
            if me ~= nil and level ~= nil then
                print(`[nakama] client account={me:GetAttribute("Account")} level={level}`)
                break
            end
            task.wait()
        end
        -- **Into Nakama's matchmaker**, for two against... one each.
        NakamaService.Matched:Connect(function(match: string)
            print(`[nakama] client matched {#match > 0}`)
        end)
        local ticket, why = NakamaService:QueueAsync(door, "Duel")
        print(`[nakama] client queued={ticket ~= nil} {why or ""}`)
        while true do
            local me = NetworkService.LocalPlayer
            if me ~= nil and me:GetAttribute("InArena") == true then
                print("[nakama] client in arena")
                return
            end
            task.wait()
        end
    end)
end
