--!strict
-- A player's machine: an account made at the front door, a sign-in and a join
-- with its ticket; its name said in each world it reaches; every chat line it
-- hears said, and the guild's line told to the server it is on; the guild's
-- line said when the town asks; and the end of its road -- the arena or the
-- dungeon -- said last.
local NetworkService = game:GetService("NetworkService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local SceneService = game:GetService("SceneService")

local chat = require("@engine/chat")

local GuildLine = "for the wardens"
local Password = "correct horse battery"

type Load = { door: string?, name: string? }
local load = (SceneService:GetLoadData() or {}) :: Load

local function remote(): RemoteEvent
    return (ReplicatedStorage:WaitForChild("E2E") :: any) :: RemoteEvent
end

if load.door ~= nil and load.name ~= nil then
    local door = load.door :: string
    local name = load.name :: string

    local heard = false
    chat.messageReceived:Connect(function(message: any)
        print(`[e2e] {name} heard {message.Channel}:{message.FromName}:{message.Text}`)
        if message.Channel == "guild" and message.Text == GuildLine and not heard then
            heard = true
            remote():FireServer("heard")
        end
    end)
    NetworkService.Connected:Connect(function()
        task.spawn(function()
            remote():FireServer("name", name)
        end)
    end)

    task.spawn(function()
        local made = NetworkService:RegisterAsync(door, name, Password)
        print(`[e2e] {name} registered ok={made.Ok}`)
        local signedIn = NetworkService:SignInAsync(door, name, Password)
        print(`[e2e] {name} signed in ok={signedIn.Ok}`)
        if signedIn.Ok then
            NetworkService:Join(door, { Ticket = signedIn.Ticket })
            chat.listen()
        end
    end)

    task.spawn(function()
        local said = false
        while true do
            local me = NetworkService.LocalPlayer
            if me ~= nil then
                if me:GetAttribute("Say") == true and not said then
                    said = true
                    chat.send("guild", GuildLine)
                end
                if me:GetAttribute("InArena") == true then
                    print(`[e2e] {name} client in arena`)
                    return
                end
                if me:GetAttribute("InDungeon") == true then
                    print(`[e2e] {name} client in dungeon`)
                    return
                end
            end
            task.wait(0.1)
        end
    end)
end
