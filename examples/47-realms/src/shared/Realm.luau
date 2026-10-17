--!strict
--[[
	What every world of the game has, for its server script: a ground in the
	world's colour, a character for each player, walking, and portals -- pads
	that do something when a character steps on one.

	**A portal is a place and a function.** Where it sends the player is the
	world's own script's to say, so the town's dungeon pad can gather a party
	and ask for an instance while the fields' pad only goes home.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

local Walk = require("@shared/Walk")

local Realm = {}

export type Portal = {
    Name: string,
    Position: vector,
    Color: Color3,
    --- Called once when a character steps on it; again only after they step off.
    Enter: (player: Player) -> (),
}

export type Options = {
    --- The world's name, which its clients show at the top of the screen.
    Name: string,
    Ground: Color3,
    Spawn: vector,
    Portals: { Portal },
}

--- How close to a pad's centre a character must stand to be on it.
Realm.PadRadius = 2

--- What a player is told, by the key of a line in `i18n/`, under the world's
--- name: their client shows it for a few seconds.
function Realm.tell(player: Player, key: string)
    player:SetAttribute("Notice", key)
    player:SetAttribute("NoticeAt", RunService.SimTime)
end

--- The players whose characters stand within `radius` of `at`.
function Realm.near(at: vector, radius: number): { Player }
    local out: { Player } = {}
    for _, player in NetworkService:GetPlayers() do
        local character = player.Character
        if character and character:IsA("BasePart") then
            local offset = character.Position - at
            if vector.magnitude(vector.create(offset.x, 0, offset.z)) <= radius then
                table.insert(out, player)
            end
        end
    end
    return out
end

function Realm.build(options: Options)
    workspace:SetAttribute("Realm", options.Name)

    local ground = Instance.new("Part")
    ground.Name = "Ground"
    ground.Anchored = true
    ground.Size = vector.create(40, 1, 40)
    ground.Position = vector.create(0, -0.5, 0)
    ground:SetMaterialParameter("Color", options.Ground)
    ground.Parent = workspace

    for _, portal in options.Portals do
        local pad = Instance.new("Part")
        pad.Name = portal.Name
        pad.Anchored = true
        pad.Size = vector.create(Realm.PadRadius * 2, 0.2, Realm.PadRadius * 2)
        pad.Position = portal.Position + vector.create(0, 0.1, 0)
        pad:SetMaterialParameter("Color", portal.Color)
        pad.Parent = workspace
        local arch = Instance.new("Part")
        arch.Name = `{portal.Name}Arch`
        arch.Anchored = true
        arch.Size = vector.create(Realm.PadRadius * 2, 3, 0.4)
        arch.Position = portal.Position + vector.create(0, 1.5, -Realm.PadRadius)
        arch:SetMaterialParameter("Color", portal.Color)
        arch.Parent = workspace
    end

    -- **A character per player**, where the world puts newcomers, gone when
    -- they leave -- for this world or another.
    local characters: { [Player]: BasePart } = {}
    local onPad: { [Player]: { [string]: boolean } } = {}
    local function spawnCharacter(player: Player)
        if characters[player] then
            return
        end
        local character = Instance.new("Part")
        character.Name = `Player{player.UserId}`
        character.Anchored = true
        character.Size = vector.create(1.2, 1.6, 1.2)
        character:SetMaterialParameter("Color", Color3.fromHSV((player.UserId * 0.37) % 1, 0.75, 1))
        local angle = player.UserId * 1.1
        character.Position = options.Spawn + vector.create(math.cos(angle) * 2, Walk.Height, math.sin(angle) * 2)
        character.Parent = workspace
        characters[player] = character
        onPad[player] = {}
        player.Character = character
    end
    for _, player in NetworkService:GetPlayers() do
        spawnCharacter(player)
    end
    NetworkService.PlayerAdded:Connect(spawnCharacter)
    NetworkService.PlayerRemoving:Connect(function(player: Player)
        local character = characters[player]
        if character then
            character:Destroy()
        end
        characters[player] = nil
        onPad[player] = nil
    end)

    RunService.Heartbeat:Connect(function(dt: number)
        for player, character in characters do
            Walk.step(player, character, dt)
            local standing = onPad[player]
            for _, portal in options.Portals do
                local offset = character.Position - portal.Position
                local on = vector.magnitude(vector.create(offset.x, 0, offset.z)) <= Realm.PadRadius
                if on and not standing[portal.Name] then
                    task.spawn(portal.Enter, player)
                end
                standing[portal.Name] = on
            end
        end
    end)
end

return Realm
