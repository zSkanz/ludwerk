--!strict
--- `@engine/npclod` -- an NPC's code, called as often as its distance from
--- the nearest player deserves (ADR 0234).
---
--- `npclod.think(guard, function(dt) ... end)` calls the function every tick
--- while a player is near the guard, every fourth tick at a middle distance,
--- every sixteenth far, and not at all while the guard is asleep
--- (`BasePart.Dormant`, which only a dedicated server sets). `dt` is the time
--- since the last call, so the code is written once and is right at every
--- rate. Calls at the same rate are spread across ticks, so a camp of forty
--- far guards does not think all on one tick.
---
--- Nothing of it is private: it reads `Workspace:GetNearestPlayerDistance`
--- and `BasePart.Dormant`, and a game that wants other rules writes them with
--- the same two.
---
--- Casing follows ADR 0034: `npclod.think` is a module function; what it
--- returns is an object, so `thinker:Stop()`.

local RunService = game:GetService("RunService")

local NpcLod = {}

export type Rates = {
    --- Under this many metres from the nearest player is near. 30.
    Near: number?,
    --- Called every this many ticks when near. 1.
    NearEvery: number?,
    --- Under this many metres is the middle distance. 120.
    Middle: number?,
    --- Called every this many ticks at the middle distance. 4.
    MiddleEvery: number?,
    --- Called every this many ticks beyond it. 16.
    FarEvery: number?,
}

export type Thinker = {
    --- What it thinks for.
    Instance: Instance,
    --- False from `Stop` on, or once the instance has left the world.
    IsThinking: boolean,
    --- Stops calling it.
    Stop: (self: Thinker) -> (),
}

type Entry = {
    thinker: Thinker,
    think: (dt: number) -> (),
    last: number,
    spread: number,
}

local Settings = {
    Near = 30,
    NearEvery = 1,
    Middle = 120,
    MiddleEvery = 4,
    FarEvery = 16,
}

local Entries: { Entry } = {}
local Ticks = 0
local Spread = 0
local Stepping: Connection? = nil

-- Where an NPC is: a part's position, a model's pivot.
local function placeOf(instance: Instance): vector?
    if instance:IsA("BasePart") then
        return (instance :: BasePart).Position
    end
    if instance:IsA("Model") then
        return (instance :: Model):GetPivot().Position
    end
    return nil
end

-- Asleep where nobody is near: the part itself, or a model's primary part.
local function asleep(instance: Instance): boolean
    if instance:IsA("BasePart") then
        return (instance :: BasePart).Dormant
    end
    if instance:IsA("Model") then
        local primary = (instance :: Model).PrimaryPart
        return primary ~= nil and primary.Dormant
    end
    return false
end

local function step()
    Ticks += 1
    local now = RunService.SimTime
    local at = 1
    while at <= #Entries do
        local entry = Entries[at]
        local instance = entry.thinker.Instance
        if not entry.thinker.IsThinking or not instance:IsDescendantOf(workspace) then
            entry.thinker.IsThinking = false
            table.remove(Entries, at)
            continue
        end
        at += 1
        local place = placeOf(instance)
        if place == nil or asleep(instance) then
            continue
        end
        local distance = workspace:GetNearestPlayerDistance(place)
        local every = if distance < Settings.Near
            then Settings.NearEvery
            elseif distance < Settings.Middle then Settings.MiddleEvery
            else Settings.FarEvery
        if (Ticks + entry.spread) % math.max(1, every) ~= 0 then
            continue
        end
        local dt = now - entry.last
        entry.last = now
        task.spawn(entry.think, dt)
    end
    if #Entries == 0 and Stepping then
        Stepping:Disconnect()
        Stepping = nil
    end
end

--- Calls `think` for `instance` -- a part, or a model by its pivot -- as
--- often as its distance from the nearest player deserves, until `Stop` or
--- until the instance leaves the world.
function NpcLod.think(instance: Instance, think: (dt: number) -> ()): Thinker
    local thinker: Thinker = {
        Instance = instance,
        IsThinking = true,
        Stop = function(self: Thinker)
            self.IsThinking = false
        end,
    }
    Spread += 1
    table.insert(Entries, { thinker = thinker, think = think, last = RunService.SimTime, spread = Spread })
    if Stepping == nil then
        Stepping = RunService.Heartbeat:Connect(step)
    end
    return thinker
end

--- Changes the distances and the rates; what is not given stays.
function NpcLod.configure(rates: Rates)
    Settings.Near = rates.Near or Settings.Near
    Settings.NearEvery = math.max(1, math.floor(rates.NearEvery or Settings.NearEvery))
    Settings.Middle = rates.Middle or Settings.Middle
    Settings.MiddleEvery = math.max(1, math.floor(rates.MiddleEvery or Settings.MiddleEvery))
    Settings.FarEvery = math.max(1, math.floor(rates.FarEvery or Settings.FarEvery))
end

return NpcLod
