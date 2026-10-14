--!strict
-- A module by its whole name, a second way to the same file, and one of the
-- engine's own by its name.

local Cards = require("./Rules/Cards.module")
local Match = require("../shared/Match")
local Same = require("../server/Rules/Cards.module")
local json = require("@std/json")

local context = Match.make()
local hero: Match.Hero = Match.hero("Ember")
print(Cards.round(context), Same.offered(hero, false))
print(json.serialize(json.deserialize('{"round": 1}')))
