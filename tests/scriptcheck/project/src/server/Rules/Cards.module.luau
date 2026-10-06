--!strict
-- Requires a module three folders away, and names it only in types: the two
-- things the editor's checker and its lint each got wrong.

local Match = require("../../shared/Match")

local Cards = {}

function Cards.round(context: Match.Context): number
    return context.Round
end

-- A value typed `any`, narrowed by an `or`, and its field read.
function Cards.offered(hero: any, ended: boolean): boolean
    if hero.Dead or hero.Body.Parent == nil or ended then
        return false
    end
    return true
end

return Cards
