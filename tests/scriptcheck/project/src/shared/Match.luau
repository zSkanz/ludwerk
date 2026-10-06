--!strict
-- A module other scripts require by path, some of them for its types alone.

export type Context = {
    Round: number,
    Players: { string },
}

export type Hero = {
    Name: string,
    Dead: boolean,
}

local Match = {}

function Match.make(): Context
    return { Round = 1, Players = {} }
end

function Match.hero(name: string): Hero
    return { Name = name, Dead = false }
end

return Match
