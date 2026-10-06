--!strict
local Curves = require("../shared/Settings/Curves")
local Match = require("../shared/Match")

local function names(context: Match.Context): string
    return table.concat(context.Players, ", ")
end

print(Curves.fade(), Curves.ember(), names(Match.make()))
