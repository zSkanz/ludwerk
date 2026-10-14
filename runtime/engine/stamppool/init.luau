--!strict
--- `@engine/stamppool` — copies of one stamp, ready before they are needed.
---
--- A game that spawns the same thing over and over -- a bullet's spark, an
--- enemy, a crate -- pays for each copy when it asks for it: the file read,
--- the tree built. A pool builds them AHEAD, so `Take` hands one over at once,
--- and refills itself after, a copy a frame, off the moment that took one.
---
--- ```lua
--- local StampPool = require("@engine/stamppool")
--- local sparks = StampPool.new("spark", 8)
---
--- local spark = sparks:Take(workspace)
--- -- ...
--- sparks:Return(spark)
--- ```
---
--- **A returned copy is not handed out again as it is**: whatever the game did
--- to it -- moved it, recoloured it, gave it children -- would come back with
--- it. It leaves the world at once, and the pool replaces it with a copy made
--- from the stamp, so what `Take` hands over is always the stamp as it is
--- (ADR 0155 §10). The stamp's file is preloaded with the pool, so no copy
--- reads it again.

local ContentProvider = game:GetService("ContentProvider")

export type StampPool = {
    --- The stamp, as `Instance.stamp` names it.
    Stamp: string,
    --- How many copies it keeps ready.
    Size: number,
    --- Its parameters, set on every copy it makes (`Instance.stamp`'s third).
    Parameters: { [string]: string | number | boolean | vector | Color3 }?,

    --- A copy, parented under `parent` when one is given: one made ahead when
    --- there is one, and one made now when there is not.
    Take: (self: StampPool, parent: Instance?) -> Instance,
    --- Gives a copy back. It leaves the world at once, and the pool makes a
    --- fresh one in its place.
    Return: (self: StampPool, copy: Instance) -> (),
    --- How many copies are ready now.
    Ready: (self: StampPool) -> number,
    --- Destroys every copy it holds; the pool makes no more.
    Destroy: (self: StampPool) -> (),
}

local PoolImpl = {}
PoolImpl.__index = PoolImpl

local function make(self: any): Instance
    return Instance.stamp(self.Stamp, true, self.Parameters)
end

-- One copy a deferred step until the stock is full again: a refill that built
-- eight at once would be the hitch the pool exists to avoid.
local function refill(self: any)
    if self.Refilling or self.Destroyed then
        return
    end
    self.Refilling = true
    task.defer(function()
        self.Refilling = false
        if self.Destroyed or #self.Stock >= self.Size then
            return
        end
        table.insert(self.Stock, make(self))
        refill(self)
    end)
end

function PoolImpl.Take(self: any, parent: Instance?): Instance
    local copy = table.remove(self.Stock) or make(self)
    if parent ~= nil then
        copy.Parent = parent
    end
    refill(self)
    return copy
end

function PoolImpl.Return(self: any, copy: Instance)
    copy:Destroy()
    refill(self)
end

function PoolImpl.Ready(self: any): number
    return #self.Stock
end

function PoolImpl.Destroy(self: any)
    self.Destroyed = true
    for _, copy in self.Stock do
        copy:Destroy()
    end
    table.clear(self.Stock)
end

local StampPool = {}

--- A pool of `size` copies of the stamp `stamp` -- four when it is not given --
--- each placed with `parameters`. The stamp is preloaded first, and the copies
--- are made over the next frames.
function StampPool.new(
    stamp: string,
    size: number?,
    parameters: { [string]: string | number | boolean | vector | Color3 }?
): StampPool
    ContentProvider:PreloadAsync({ stamp })
    local pool = setmetatable({
        Stamp = stamp,
        Size = math.max(0, math.floor(size or 4)),
        Parameters = parameters,
        Stock = {} :: { Instance },
        Refilling = false,
        Destroyed = false,
    }, PoolImpl)
    refill(pool)
    return (pool :: any) :: StampPool
end

return StampPool
