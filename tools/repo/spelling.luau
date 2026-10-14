--!strict
--[[
	The spelling of what a person reads: American, one way for each word.

	The API has always been American -- `Color3`, `Center`, `normalize` -- and
	the text around it was not, so a page said `Color` and "colour" in one
	sentence. This is the list `proselint.luau` holds the descriptions, the
	manual and the catalogs to, and the one its `--fix` writes from: a word on
	the left is written as the word on the right.

	Whole words, lower case; a capital is kept by whoever applies it. Where both
	spellings are American for different things, the word is left out rather
	than guessed at: `disc` (a round shape) beside `disk`, `dialogue` (speech)
	beside `dialog`, `cancellation`, which has two l's on both sides.

	Not for decision records, ledgers, comments or commit messages: those are
	ours, and nobody reads them in a tooltip.
]]

local Words: { [string]: string } = {}

--- One word, and each ending it takes: `family("colour", "color", { "", "s" })`.
local function family(british: string, american: string, endings: { string })
    for _, ending in endings do
        Words[british .. ending] = american .. ending
    end
end

local Noun = { "", "s" }
local Verb = { "", "s", "ed", "ing" }

-- -our
family("colour", "color", { "", "s", "ed", "ing", "ful", "less", "ings" })
family("behaviour", "behavior", { "", "s", "al" })
family("recolour", "recolor", { "", "s", "ed", "ing" })
family("discolour", "discolor", { "", "s", "ed", "ing", "ation" })
family("neighbour", "neighbor", { "", "s", "ing", "hood", "hoods" })
family("favour", "favor", { "", "s", "ed", "ing", "able", "ite", "ites" })
family("honour", "honor", Verb)
family("armour", "armor", { "", "s", "ed" })
family("flavour", "flavor", Verb)
family("labour", "labor", Verb)
family("rumour", "rumor", Noun)
family("vapour", "vapor", Noun)
family("harbour", "harbor", Verb)
family("humour", "humor", Noun)

-- -re
Words["centre"] = "center"
Words["centres"] = "centers"
Words["centred"] = "centered"
Words["centring"] = "centering"
for _, prefix in { "", "kilo", "centi", "milli", "micro", "nano" } do
    family(prefix .. "metre", prefix .. "meter", Noun)
end
family("litre", "liter", Noun)
family("fibre", "fiber", Noun)
family("theatre", "theater", Noun)
family("calibre", "caliber", Noun)
family("lustre", "luster", Noun)
family("manoeuvre", "maneuver", { "", "s", "d" })
Words["manoeuvring"] = "maneuvering"
Words["sombre"] = "somber"

-- -ise, -yse
local Ise = { "e", "es", "ed", "ing", "ation", "ations", "er", "ers" }
for _, stem in
    {
        "normal",
        "initial",
        "organ",
        "recogn",
        "optim",
        "synchron",
        "serial",
        "deserial",
        "visual",
        "custom",
        "minim",
        "maxim",
        "local",
        "raster",
        "priorit",
        "summar",
        "util",
        "real",
        "final",
        "emphas",
        "categor",
        "special",
        "general",
        "random",
        "stabil",
        "penal",
        "character",
        "parameter",
        "memor",
        "author",
        "capital",
        "styl",
        "symbol",
        "token",
        "vector",
        "standard",
        "central",
        "quant",
        "linear",
        "polar",
        "apolog",
        "critic",
        "harmon",
        "internal",
        "external",
        "material",
        "modern",
        "neutral",
        "equal",
    }
do
    family(stem .. "is", stem .. "iz", Ise)
end
-- `colourise` loses its `u` as well.
family("colouris", "coloriz", Ise)
family("analys", "analyz", { "e", "es", "ed", "ing", "er", "ers" })
family("paralys", "paralyz", { "e", "es", "ed", "ing" })

-- A doubled l before an ending.
for _, stem in
    {
        "travel",
        "model",
        "label",
        "level",
        "signal",
        "fuel",
        "channel",
        "tunnel",
        "bevel",
        "jewel",
        "marshal",
        "total",
        "dial",
        "equal",
        "initial",
        "pencil",
        "shovel",
        "spiral",
        "swivel",
    }
do
    Words[stem .. "led"] = stem .. "ed"
    Words[stem .. "ling"] = stem .. "ing"
    Words[stem .. "ler"] = stem .. "er"
    Words[stem .. "lers"] = stem .. "ers"
end
Words["cancelled"] = "canceled"
Words["cancelling"] = "canceling"

-- -ence, -ogue
family("licence", "license", Noun)
family("defence", "defense", Noun)
family("offence", "offense", Noun)
family("pretence", "pretense", Noun)
family("catalogue", "catalog", { "", "s", "d" })
family("analogue", "analog", Noun)

-- -wards, and the rest one at a time.
for _, direction in { "to", "after", "back", "for", "up", "down", "out", "in", "on", "side", "sky", "left", "right" } do
    Words[direction .. "wards"] = direction .. "ward"
end
Words["grey"] = "gray"
Words["greys"] = "grays"
Words["greyed"] = "grayed"
Words["greyscale"] = "grayscale"
Words["whilst"] = "while"
Words["amongst"] = "among"
Words["learnt"] = "learned"
Words["spelt"] = "spelled"
Words["tyre"] = "tire"
Words["tyres"] = "tires"
Words["kerb"] = "curb"
Words["kerbs"] = "curbs"
Words["storey"] = "story"
Words["storeys"] = "stories"
Words["aluminium"] = "aluminum"
Words["artefact"] = "artifact"
Words["artefacts"] = "artifacts"
Words["programme"] = "program"
Words["programmes"] = "programs"
Words["anticlockwise"] = "counterclockwise"
Words["mould"] = "mold"
Words["moulds"] = "molds"
Words["plough"] = "plow"
Words["sceptical"] = "skeptical"
Words["ageing"] = "aging"
Words["focussed"] = "focused"
Words["focussing"] = "focusing"
Words["skilful"] = "skillful"
Words["fulfil"] = "fulfill"
Words["enrol"] = "enroll"
Words["cosy"] = "cozy"
Words["draught"] = "draft"
Words["practise"] = "practice"
Words["practised"] = "practiced"
Words["practising"] = "practicing"
Words["judgement"] = "judgment"
Words["acknowledgement"] = "acknowledgment"

return Words
