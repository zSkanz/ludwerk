--!strict
--- `@engine/chat` -- an online game's chat (ADR 0239), on pieces every game
--- has: a `RemoteEvent`, `CrossWorldService` between worlds, the party and
--- guild a player is in, and the console's mute.
---
--- On the server, `chat.start(config)` once. On a player's machine,
--- `chat.messageReceived:Connect` hears each line, `chat.noticeReceived` why
--- one of theirs went nowhere, and `chat.send` and `chat.whisper` speak.
---
--- - `local`: the players whose characters are within `LocalRadius` of the
---   speaker's, in this world.
--- - `world`: everyone in this world and its channels.
--- - `party` and `guild`: the members of the speaker's party or guild, in
---   whatever world each is.
--- - `whisper`: one player by name, wherever they are; `NotOnline` when no
---   world has them.
---
--- A line is 200 characters at most. Five in five seconds a player; past
--- that a line is dropped and its speaker told `TooFast`. A muted player's
--- lines reach nobody (`Muted`), and the game's `Filter` sees each line
--- before it goes anywhere: nil drops it. Joining, a player is sent the last
--- `History` lines of the world channel, and of their party's and guild's.
--- A machine hears each line once: one it already heard, sent again with a
--- world's history after a hop, is not passed on.
---
--- Casing follows ADR 0034: `chat.start` is a module function, and a line is
--- an object: `message.Channel`, `message.FromName`, `message.Text`.

local CrossWorldService = game:GetService("CrossWorldService")
local DataService = game:GetService("DataService")
local NetworkService = game:GetService("NetworkService")
local PartyService = game:GetService("PartyService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")

local Chat = {}

export type Message = {
    Channel: string,
    FromName: string,
    Text: string,
    --- Which line this is, wherever it is sent again: made where it is said.
    Id: string?,
}

export type Config = {
    --- The channels this game has, of `local`, `party`, `guild`, `world`
    --- and `whisper`. All five when absent.
    Channels: { string }?,
    --- Meters, for `local`. 60.
    LocalRadius: number?,
    --- Lines sent on joining, a channel. 50.
    History: number?,
    --- The game's own word on each line, on the server of the world it was
    --- sent in: what to send instead, or nil to drop it.
    Filter: ((text: string, from: Player) -> string?)?,
}

export type Connection = { Disconnect: (self: Connection) -> () }
export type MessageSignal = { Connect: (self: MessageSignal, callback: (message: Message) -> ()) -> Connection }
export type NoticeSignal = { Connect: (self: NoticeSignal, callback: (notice: string) -> ()) -> Connection }

local RemoteName = "EngineChat"
local LongestLine = 200
local LinesPerWindow = 5
local WindowSeconds = 5
local WhisperWaitSeconds = 2
local HistoryScope = "chat"
-- How many lines a machine remembers having heard: more than one channel's
-- history, so a hop's history is all known.
local RememberedLines = 512

-- **A signal of a module's own**: each callback called deferred (R8).
local function signal(): any
    local listeners: { [number]: (...any) -> () } = {}
    local next = 0
    local made = {}
    function made.Connect(_self: any, callback: (...any) -> ()): Connection
        next += 1
        local at = next
        listeners[at] = callback
        local connection = {}
        function connection.Disconnect(_: any)
            listeners[at] = nil
        end
        return connection :: any
    end
    function made.fire(...: any)
        for _, callback in listeners do
            task.defer(callback, ...)
        end
    end
    return made
end

local Received = signal()
local Noticed = signal()
--- A line this machine's player was sent.
Chat.messageReceived = Received :: MessageSignal
--- Why a line of this machine's player went nowhere: `TooFast`, `Muted`,
--- `NotOnline`, `NoParty`, `NoGuild`, `Unknown` (a channel the game does not
--- have).
Chat.noticeReceived = Noticed :: NoticeSignal

local function remote(): RemoteEvent?
    local found = ReplicatedStorage:FindFirstChild(RemoteName)
    return if found and found:IsA("RemoteEvent") then found :: RemoteEvent else nil
end

-- ---------------------------------------------------------------------------
-- A player's machine.

-- **What this machine hears**: one connection, to the remote of the world it
-- is in, made again when a hop brings another -- and a line still on its way
-- from the one before ignored.
local Generation = 0
-- **The lines this machine has heard**, by id, the oldest forgotten first.
local HeardIds: { [string]: boolean } = {}
local HeardOrder: { string } = {}

local function heardBefore(id: any): boolean
    if typeof(id) ~= "string" then
        return false
    end
    if HeardIds[id] then
        return true
    end
    HeardIds[id] = true
    table.insert(HeardOrder, id)
    if #HeardOrder > RememberedLines then
        HeardIds[table.remove(HeardOrder, 1) :: string] = nil
    end
    return false
end
local Hearing: { Disconnect: (any) -> () }? = nil
local Heard: RemoteEvent? = nil
local Following = false

local function channelOnClient(): RemoteEvent
    local found = remote()
    if found then
        return found
    end
    -- The server makes it; it may not have arrived yet.
    local child = ReplicatedStorage:WaitForChild(RemoteName)
    return (child :: any) :: RemoteEvent
end

local function connect(channel: RemoteEvent)
    if channel == Heard then
        return
    end
    Heard = channel
    Generation += 1
    local mine = Generation
    if Hearing then
        Hearing:Disconnect()
    end
    Hearing = channel.ClientReceived:Connect(function(kind: any, payload: any)
        if mine ~= Generation then
            return
        end
        if kind == "line" and typeof(payload) == "table" then
            if not heardBefore(payload.Id) then
                Received.fire(payload)
            end
        elseif kind == "notice" and typeof(payload) == "string" then
            Noticed.fire(payload)
        end
    end)
end

local function listen(channel: RemoteEvent)
    if Following then
        return
    end
    Following = true
    connect(channel)
    -- **After every hop**, the next world's remote, once it is here.
    NetworkService.Connected:Connect(function()
        task.spawn(function()
            connect(channelOnClient())
        end)
    end)
end

--- Says `text` in `channel`, from this machine's player.
function Chat.send(channel: string, text: string)
    local remoteEvent = channelOnClient()
    listen(remoteEvent)
    remoteEvent:FireServer("send", channel, text)
end

--- Says `text` to one player, by name, wherever they are.
function Chat.whisper(name: string, text: string)
    local remoteEvent = channelOnClient()
    listen(remoteEvent)
    remoteEvent:FireServer("whisper", name, text)
end

--- Hears lines without saying one first.
function Chat.listen()
    listen(channelOnClient())
end

-- ---------------------------------------------------------------------------
-- The server.

local Started = false
-- **The remote of the scene this server runs**, and what listens to it. A
-- scene changed -- an instance given its match, a `LoadScene` -- takes its
-- remote with it, and the scene's script calls `chat.start` again: what was
-- subscribed between worlds stays, and the remote and its listeners are made
-- again.
local Current: RemoteEvent? = nil
local Bound: { { Disconnect: (any) -> () } } = {}
local Bind: ((channel: RemoteEvent) -> ())? = nil

local function remoteHere(): RemoteEvent
    local found = remote()
    if found then
        return found
    end
    local made = Instance.new("RemoteEvent")
    made.Name = RemoteName
    made.Parent = ReplicatedStorage
    return made
end

--- Starts the chat on this server; the server of every world of the game
--- calls it with the same config. Called again -- by a scene loaded since --
--- it keeps the config it was first given and takes up the new scene.
function Chat.start(config: Config?)
    assert(NetworkService.Authority, "chat.start runs on the server")
    if Started then
        local channel = remoteHere()
        if channel ~= Current and Bind then
            (Bind :: (RemoteEvent) -> ())(channel)
        end
        return
    end
    Started = true
    local settings: Config = config or {}
    local enabled: { [string]: boolean } = {}
    for _, name in settings.Channels or { "local", "party", "guild", "world", "whisper" } do
        enabled[name] = true
    end
    local radius = settings.LocalRadius or 60
    local keep = math.max(0, math.floor(settings.History or 50))
    local filter = settings.Filter
    local guild = if enabled["guild"] then require("@engine/guild") else nil

    local worldTopic = "engine.chat.world." .. NetworkService.WorldName
    local worldHistory: { Message } = {}
    -- When each player's last lines were said, for the rate.
    local said: { [Player]: { number } } = {}
    -- Whispers this world sent, waiting to be heard of.
    local waiting: { [string]: Player } = {}

    local function tell(player: Player, notice: string)
        if Current then
            Current:FireClient(player, "notice", notice)
        end
    end

    local function deliver(player: Player, line: Message)
        if Current then
            Current:FireClient(player, "line", line)
        end
    end

    local function remember(list: { Message }, line: Message)
        table.insert(list, line)
        while #list > keep do
            table.remove(list, 1)
        end
    end

    -- **Kept in the data service** for a party or a guild, so a member
    -- joining any world is sent it.
    local function keepFor(key: string, line: Message)
        if keep == 0 then
            return
        end
        DataService:UpdateAsync(HistoryScope, key, function(old: any): any
            local list: { Message } = if typeof(old) == "table" then table.clone(old) else {}
            remember(list, line)
            return list
        end)
    end

    local function sendHistory(player: Player, key: string)
        local list = DataService:GetAsync(HistoryScope, key)
        if typeof(list) == "table" then
            for _, line in list :: { Message } do
                deliver(player, line)
            end
        end
    end

    local function members(list: { string }): { [string]: boolean }
        local set: { [string]: boolean } = {}
        for _, member in list do
            set[member] = true
        end
        return set
    end

    local function deliverToMembers(set: { [string]: boolean }, line: Message)
        for _, player in NetworkService:GetPlayers() do
            if set[PartyService:MemberIdOf(player)] then
                deliver(player, line)
            end
        end
    end

    -- **Between worlds**: each world delivers to its own players.
    if enabled["world"] then
        CrossWorldService:Subscribe(worldTopic, function(message: any, _origin: string)
            if typeof(message) ~= "table" then
                return
            end
            local line = message :: Message
            remember(worldHistory, line)
            for _, player in NetworkService:GetPlayers() do
                deliver(player, line)
            end
        end)
    end
    if enabled["party"] then
        CrossWorldService:Subscribe("engine.chat.party", function(message: any, _origin: string)
            if typeof(message) == "table" and typeof(message.Members) == "table" then
                deliverToMembers(members(message.Members), message.Line)
            end
        end)
    end
    if enabled["guild"] then
        CrossWorldService:Subscribe("engine.chat.guild", function(message: any, _origin: string)
            if typeof(message) == "table" and typeof(message.Members) == "table" then
                deliverToMembers(members(message.Members), message.Line)
            end
        end)
    end
    if enabled["whisper"] then
        CrossWorldService:Subscribe("engine.chat.whisper", function(message: any, _origin: string)
            if typeof(message) ~= "table" or typeof(message.To) ~= "string" then
                return
            end
            for _, player in NetworkService:GetPlayers() do
                if player.Name == message.To then
                    deliver(player, message.Line)
                    CrossWorldService:Publish("engine.chat.whispered", message.Token)
                    return
                end
            end
        end)
        CrossWorldService:Subscribe("engine.chat.whispered", function(message: any, _origin: string)
            if typeof(message) == "string" then
                waiting[message] = nil
            end
        end)
    end

    -- **Five lines in five seconds** a player, by the simulation's clock.
    local function tooFast(player: Player): boolean
        local now = RunService.SimTime
        local times = said[player] or {}
        local kept: { number } = {}
        for _, at in times do
            if now - at < WindowSeconds then
                table.insert(kept, at)
            end
        end
        if #kept >= LinesPerWindow then
            said[player] = kept
            return true
        end
        table.insert(kept, now)
        said[player] = kept
        return false
    end

    -- What every line passes before it goes anywhere: the rate, the mute,
    -- the length and the game's filter.
    local function admit(player: Player, text: any): string?
        if typeof(text) ~= "string" or #text == 0 then
            return nil
        end
        if tooFast(player) then
            tell(player, "TooFast")
            return nil
        end
        if NetworkService:IsMuted(player) then
            tell(player, "Muted")
            return nil
        end
        local line = string.sub(text, 1, LongestLine)
        if filter then
            local ok, kept = pcall(filter, line, player)
            if not ok or typeof(kept) ~= "string" then
                return nil
            end
            line = string.sub(kept, 1, LongestLine)
        end
        return line
    end

    -- **A line's id**: this world, the tick, a count and a draw, so a world
    -- started again names no line as one of before.
    local linesSaid = 0
    local function lineId(): string
        linesSaid += 1
        return `{NetworkService.WorldName}:{RunService.SimTime}:{linesSaid}:{math.random()}`
    end

    local function onReceived(player: Player, kind: any, first: any, second: any)
        if kind == "whisper" then
            if not enabled["whisper"] or typeof(first) ~= "string" then
                tell(player, "Unknown")
                return
            end
            local text = admit(player, second)
            if text == nil then
                return
            end
            local line: Message = { Channel = "whisper", FromName = player.Name, Text = text, Id = lineId() }
            local token = player.Name .. ":" .. tostring(RunService.SimTime) .. ":" .. tostring(math.random())
            waiting[token] = player
            deliver(player, line)
            CrossWorldService:Publish("engine.chat.whisper", { To = first, Line = line, Token = token })
            task.delay(WhisperWaitSeconds, function()
                if waiting[token] ~= nil then
                    waiting[token] = nil
                    tell(player, "NotOnline")
                end
            end)
            return
        end
        if kind ~= "send" or typeof(first) ~= "string" or not enabled[first] then
            tell(player, "Unknown")
            return
        end
        local text = admit(player, second)
        if text == nil then
            return
        end
        local line: Message = { Channel = first, FromName = player.Name, Text = text, Id = lineId() }
        if first == "local" then
            local character = player.Character
            local here = if character and character:IsA("BasePart") then (character :: BasePart).Position else nil
            for _, other in NetworkService:GetPlayers() do
                local theirs = other.Character
                local near = other == player
                if not near and here and theirs and theirs:IsA("BasePart") then
                    near = vector.magnitude((theirs :: BasePart).Position - here) <= radius
                end
                if near then
                    deliver(other, line)
                end
            end
        elseif first == "world" then
            CrossWorldService:Publish(worldTopic, line)
        elseif first == "party" then
            local party = PartyService:GetPartyAsync(player)
            if party == nil then
                tell(player, "NoParty")
                return
            end
            CrossWorldService:Publish("engine.chat.party", { Members = party.Members, Line = line })
            keepFor("party:" .. party.Id, line)
        elseif first == "guild" and guild then
            local id = guild.of(player)
            local info = if id then guild.get(id) else nil
            if id == nil or info == nil then
                tell(player, "NoGuild")
                return
            end
            local list: { string } = {}
            for member in info.Members do
                table.insert(list, member)
            end
            CrossWorldService:Publish("engine.chat.guild", { Members = list, Line = line })
            keepFor("guild:" .. id, line)
        end
    end

    -- **Joining**: the world's last lines, and the party's and guild's.
    local function onAdded(player: Player)
        if keep == 0 then
            return
        end
        for _, line in worldHistory do
            deliver(player, line)
        end
        if enabled["party"] then
            local party = PartyService:GetPartyAsync(player)
            if party then
                sendHistory(player, "party:" .. party.Id)
            end
        end
        if enabled["guild"] and guild then
            local id = guild.of(player)
            if id then
                sendHistory(player, "guild:" .. id)
            end
        end
    end

    local function bind(channel: RemoteEvent)
        for _, connection in Bound do
            connection:Disconnect()
        end
        Current = channel
        Bound = {
            channel.ServerReceived:Connect(onReceived) :: any,
            NetworkService.PlayerAdded:Connect(onAdded) :: any,
            NetworkService.PlayerRemoving:Connect(function(player: Player)
                said[player] = nil
            end) :: any,
        }
    end
    Bind = bind
    bind(remoteHere())
end

return Chat
