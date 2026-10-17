--!strict
--[[
	The town (`servers.toml`'s entry world): where every player arrives, with a
	road to the fields and the dungeon's gate.

	- **The road** is `NetworkService:Travel`: the player's machine leaves this
	  world for the fields, behind the loading screen `@engine/travelscreen`
	  draws, and the fields' `TravelArrived` says they came from here.
	- **The gate** gathers a party -- everyone standing on it -- and asks the
	  manager for a dungeon of their own with `InstanceService:CreateInstanceAsync`,
	  then sends them together with `TravelGroup`. Only they may enter it.
	- **When a dungeon is gone**, `InstanceClosed` says why: closed after its
	  party left, or fallen. A player sent back from one that fell arrives with
	  `reason` `"InstanceFailed"`, and is told.

	Alone -- with no manager -- there is no other world to go to, and both pads
	say so rather than raise.
]]

local InstanceService = game:GetService("InstanceService")
local NetworkService = game:GetService("NetworkService")

local Realm = require("@shared/Realm")

local GatePosition = vector.create(10, 0, -10)
local gathering = false

local function toFields(player: Player)
    local ok = pcall(function()
        NetworkService:Travel(player, "fields", { Gift = "a lantern from the town" })
    end)
    if not ok then
        Realm.tell(player, "realm.notice.needs_servers")
    end
end

local function toDungeon(player: Player)
    if gathering then
        return
    end
    gathering = true
    -- **The party** is everyone on the gate when the first steps on it.
    local party = Realm.near(GatePosition, Realm.PadRadius + 1)
    for _, member in party do
        Realm.tell(member, "realm.notice.opening_dungeon")
    end
    local names: { string } = {}
    for _, member in party do
        table.insert(names, member.Name)
    end
    local made, id = pcall(function()
        return InstanceService:CreateInstanceAsync("dungeon", { Players = party, Data = { Party = names } })
    end)
    if made then
        NetworkService:TravelGroup(party, id :: string, { From = "town" })
    else
        for _, member in party do
            Realm.tell(member, "realm.notice.needs_servers")
        end
    end
    gathering = false
end

Realm.build({
    Name = "town",
    Ground = Color3.fromRGB(176, 160, 128),
    Spawn = vector.create(0, 0, 6),
    Portals = {
        { Name = "Road", Position = vector.create(-10, 0, -10), Color = Color3.fromRGB(96, 180, 96), Enter = toFields },
        { Name = "Gate", Position = GatePosition, Color = Color3.fromRGB(140, 90, 200), Enter = toDungeon },
    },
})

NetworkService.TravelArrived:Connect(function(player: Player, _data: any, origin: string, reason: string)
    if reason == "InstanceFailed" then
        Realm.tell(player, "realm.notice.dungeon_fell")
    elseif origin ~= "" then
        Realm.tell(player, "realm.notice.welcome_back")
    end
end)

InstanceService.InstanceClosed:Connect(function(id: string, reason: string)
    print(`dungeon {id} is gone: {reason}`)
end)
