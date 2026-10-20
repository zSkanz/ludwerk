--!strict
--- `@engine/guild` -- a guild is data (ADR 0240 §2): a name, members with
--- ranks, and what each rank may do, kept in the data service's shared values
--- (scope `guild`) so that every world of a game sees one guild, and two
--- worlds changing it at once both keep their change.
---
--- Permissions are strings the game names (`"invite"`, `"bank.withdraw"`);
--- this module checks them and does nothing else with them. It checks three
--- of its own: `invite` to invite, `kick` to remove a member, `rank` to give
--- a member a rank. A guild's founder has the rank `Leader`, which may do
--- everything (`"*"`); a member who joins has `Member`, which may do nothing
--- until the game says otherwise. A member is who `PartyService:MemberIdOf`
--- says a player is: their account, or their key.
---
--- Server side only. Casing follows ADR 0034: `guild.create` is a module
--- function.

local CrossWorldService = game:GetService("CrossWorldService")
local CryptoService = game:GetService("CryptoService")
local DataService = game:GetService("DataService")
local PartyService = game:GetService("PartyService")

local Guild = {}

export type Info = {
    Id: string,
    Name: string,
    --- Each rank and what it may do.
    Ranks: { [string]: { string } },
    --- Each member and their rank.
    Members: { [string]: string },
    --- Who was invited and has not answered.
    Invited: { [string]: boolean },
}

export type Connection = { Disconnect: (self: Connection) -> () }
export type Signal = { Connect: (self: Signal, callback: (id: string) -> ()) -> Connection }

local Scope = "guild"
local Topic = "engine.guild.changed"
local LeaderRank = "Leader"
local MemberRank = "Member"
local LongestName = 32

local Listeners: { [number]: (id: string) -> () } = {}
local NextListener = 0
local Subscribed = false

-- **Every world hears a guild changed**, this one included: the topic
-- carries it, and each listener is called deferred (R8).
local function listen()
    if Subscribed then
        return
    end
    Subscribed = true
    CrossWorldService:Subscribe(Topic, function(message: any, _origin: string)
        if typeof(message) ~= "string" then
            return
        end
        for _, callback in Listeners do
            task.defer(callback, message)
        end
    end)
end

local changedSignal = {}
function changedSignal.Connect(_self: any, callback: (id: string) -> ()): Connection
    listen()
    NextListener += 1
    local at = NextListener
    Listeners[at] = callback
    local connection = {}
    function connection.Disconnect(_: any)
        Listeners[at] = nil
    end
    return connection :: any
end
--- Fires in every world with a guild's id when it changes: a member joins,
--- leaves or is given a rank, or a rank is added.
Guild.changed = (changedSignal :: any) :: Signal

local function changed(id: string)
    listen()
    CrossWorldService:Publish(Topic, id)
end

local function memberOf(who: Player | string): string
    if typeof(who) == "string" then
        return who
    end
    return PartyService:MemberIdOf(who :: Player)
end

local function read(id: string): Info?
    local value = DataService:GetAsync(Scope, id)
    if typeof(value) ~= "table" then
        return nil
    end
    return value :: Info
end

-- A rank's permission, `*` meaning all of them. A claim let go of is `false`:
-- only a string names a guild.
local function allows(info: Info, member: string, permission: string): boolean
    local rank = info.Members[member]
    if rank == nil then
        return false
    end
    for _, allowed in info.Ranks[rank] or {} do
        if allowed == "*" or allowed == permission then
            return true
        end
    end
    return false
end

-- **A change to one guild**, retried by the data service where another
-- world wrote first; `change` returns the reason it refuses, or nil.
local function update(id: string, change: (info: Info) -> string?): (boolean, string?)
    local refused: string? = nil
    local found = false
    DataService:UpdateAsync(Scope, id, function(old: any): any
        refused = nil
        found = typeof(old) == "table"
        if not found then
            return old
        end
        local info = table.clone(old :: Info)
        info.Ranks = table.clone(info.Ranks)
        info.Members = table.clone(info.Members)
        info.Invited = table.clone(info.Invited)
        refused = change(info)
        if refused ~= nil then
            return old
        end
        return info
    end)
    if not found then
        return false, "NoGuild"
    end
    if refused ~= nil then
        return false, refused
    end
    changed(id)
    return true, nil
end

--- Makes a guild named `name`, `founder` its `Leader`, and returns its id;
--- or nil and `Taken` (another guild has the name, in any case), `InGuild`
--- or `Invalid`.
function Guild.create(founder: Player, name: string): (string?, string?)
    if #name == 0 or #name > LongestName then
        return nil, "Invalid"
    end
    local member = memberOf(founder)
    if typeof((DataService:GetAsync(Scope, "member:" .. member))) == "string" then
        return nil, "InGuild"
    end
    local id = "g-" .. CryptoService:UniqueId()
    -- The name is claimed first: the first world to write it has it.
    local holder = DataService:UpdateAsync(Scope, "name:" .. string.lower(name), function(old: any): any
        return if typeof(old) == "string" then old else id
    end)
    if holder ~= id then
        return nil, "Taken"
    end
    local owner = DataService:UpdateAsync(Scope, "member:" .. member, function(old: any): any
        return if typeof(old) == "string" then old else id
    end)
    if owner ~= id then
        return nil, "InGuild"
    end
    local info: Info = {
        Id = id,
        Name = name,
        Ranks = { [LeaderRank] = { "*" }, [MemberRank] = {} },
        Members = { [member] = LeaderRank },
        Invited = {},
    }
    DataService:SetAsync(Scope, id, info, 0)
    changed(id)
    return id, nil
end

--- A guild as it is now, or nil.
function Guild.get(id: string): Info?
    return read(id)
end

--- The guild a player or member is in, or nil.
function Guild.of(who: Player | string): string?
    local id = DataService:GetAsync(Scope, "member:" .. memberOf(who))
    return if typeof(id) == "string" then id else nil
end

--- Adds a rank, or says again what an existing one may do.
function Guild.addRank(id: string, rank: string, permissions: { string }): (boolean, string?)
    return update(id, function(info: Info): string?
        info.Ranks[rank] = table.clone(permissions)
        return nil
    end)
end

--- Whether a member's rank may do `permission`.
function Guild.can(id: string, who: Player | string, permission: string): boolean
    local info = read(id)
    return info ~= nil and allows(info, memberOf(who), permission)
end

--- Invites a player, here or by member id, on behalf of `by`, whose rank must
--- have `invite`. Refused with `NotAllowed`, `InGuild` or `NoGuild`.
function Guild.invite(id: string, by: Player, invitee: Player | string): (boolean, string?)
    local asker = memberOf(by)
    local member = memberOf(invitee)
    if Guild.of(member) ~= nil then
        return false, "InGuild"
    end
    return update(id, function(info: Info): string?
        if not allows(info, asker, "invite") then
            return "NotAllowed"
        end
        info.Invited[member] = true
        return nil
    end)
end

--- Puts an invited player into the guild, with the rank `Member`. Refused
--- with `NotInvited` or `InGuild`.
function Guild.join(id: string, invitee: Player): (boolean, string?)
    local member = memberOf(invitee)
    local owner = DataService:UpdateAsync(Scope, "member:" .. member, function(old: any): any
        return if typeof(old) == "string" then old else id
    end)
    if owner ~= id then
        return false, "InGuild"
    end
    local ok, why = update(id, function(info: Info): string?
        if not info.Invited[member] then
            return "NotInvited"
        end
        info.Invited[member] = nil
        info.Members[member] = MemberRank
        return nil
    end)
    if not ok then
        -- Not let in: the claim on the player goes.
        DataService:UpdateAsync(Scope, "member:" .. member, function(old: any): any
            return if old == id then false else old
        end)
    end
    return ok, why
end

local function remove(id: string, member: string, by: string?): (boolean, string?)
    local ok, why = update(id, function(info: Info): string?
        if info.Members[member] == nil then
            return "NotMember"
        end
        if by ~= nil and not allows(info, by, "kick") then
            return "NotAllowed"
        end
        info.Members[member] = nil
        return nil
    end)
    if ok then
        DataService:UpdateAsync(Scope, "member:" .. member, function(old: any): any
            return if old == id then false else old
        end)
    end
    return ok, why
end

--- Takes a player out of their guild.
function Guild.leave(who: Player | string): (boolean, string?)
    local member = memberOf(who)
    local id = Guild.of(member)
    if id == nil then
        return false, "NotMember"
    end
    return remove(id, member, nil)
end

--- Removes a member, online or not, on behalf of `by`, whose rank must have
--- `kick`.
function Guild.kick(id: string, by: Player, member: Player | string): (boolean, string?)
    return remove(id, memberOf(member), memberOf(by))
end

--- Gives a member a rank the guild has, on behalf of `by`, whose rank must
--- have `rank`. Refused with `NotAllowed`, `NotMember` or `NoRank`.
function Guild.setRank(id: string, by: Player, who: Player | string, rank: string): (boolean, string?)
    local asker = memberOf(by)
    local member = memberOf(who)
    return update(id, function(info: Info): string?
        if not allows(info, asker, "rank") then
            return "NotAllowed"
        end
        if info.Members[member] == nil then
            return "NotMember"
        end
        if info.Ranks[rank] == nil then
            return "NoRank"
        end
        info.Members[member] = rank
        return nil
    end)
end

return Guild
