--!strict
--- Conformance: `@std/json` (docs/api-design.md §7, ADR 0030).
---
--- The SHAPE: that the module resolves and exports what
--- `runtime/std/json/init.luau` declares, which is Lute's surface name for
--- name, and nothing else. The CONTRACT: what a value is written as, that the
--- text reads back as the value, and that what JSON cannot say is refused.

local json = require("@std/json")
local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("@std/json", function()
    it("resolves, is the same table every time, and is frozen", function()
        expect(json):ToBeTruthy()
        expect(require("@std/json")):ToBe(json)
        expect(function()
            (json :: any).parse = function() end
        end):ToThrow()
    end)

    it("exports Lute's six names and no others", function()
        local names = {}
        for name in json :: any do
            table.insert(names, name)
        end
        table.sort(names)
        expect(table.concat(names, " ")):ToBe("asArray asObject deserialize null object serialize")
    end)

    describe("serialize", function()
        it("writes the plain values", function()
            expect(json.serialize(true)):ToBe("true")
            expect(json.serialize(false)):ToBe("false")
            expect(json.serialize(json.null)):ToBe("null")
            expect(json.serialize(12)):ToBe("12")
            expect(json.serialize(-3)):ToBe("-3")
            expect(json.serialize(0.5)):ToBe("0.5")
            expect(json.serialize("word")):ToBe('"word"')
        end)

        it("writes a number so that it reads back the same", function()
            for _, value in { 0.1, 1 / 3, 123456789.125, 1e21, 2 ^ 53 - 1, -1.5e-7 } do
                expect(json.deserialize(json.serialize(value))):ToBe(value)
            end
        end)

        it("escapes what a string may not hold bare", function()
            expect(json.serialize('say "hi"\\\n\ttab')):ToBe('"say \\"hi\\"\\\\\\n\\ttab"')
            expect(json.serialize("\1")):ToBe('"\\u0001"')
            -- Bytes past ASCII go as they are: the text is UTF-8.
            expect(json.serialize("ação")):ToBe('"ação"')
        end)

        it("writes an array in order and an object by its members' names", function()
            local mixed: { any } = { 3, "two", true }
            expect(json.serialize(mixed)):ToBe('[3,"two",true]')
            expect(json.serialize({ b = 2, a = 1, c = { 1, 2 } })):ToBe('{"a":1,"b":2,"c":[1,2]}')
            -- The same table, the same text, whatever order it was filled in.
            local one = { zeta = 1, alpha = 2, mid = 3 }
            local other = {}
            other.mid = 3
            other.alpha = 2
            other.zeta = 1
            expect(json.serialize(one)):ToBe(json.serialize(other))
        end)

        it("takes an empty table for an array, and json.object for an empty object", function()
            expect(json.serialize({})):ToBe("[]")
            expect(json.serialize(json.object({}))):ToBe("{}")
            expect(json.serialize({ list = {}, map = json.object({}) })):ToBe('{"list":[],"map":{}}')
        end)

        it("indents when asked", function()
            expect(json.serialize({ a = { 1, 2 }, b = json.object({}) }, true)):ToBe(
                '{\n    "a": [\n        1,\n        2\n    ],\n    "b": {}\n}'
            )
        end)

        it("refuses what JSON cannot say", function()
            expect(function()
                json.serialize(0 / 0)
            end):ToThrow()
            expect(function()
                json.serialize(math.huge)
            end):ToThrow()
            expect(function()
                json.serialize({ [true] = 1 } :: any)
            end):ToThrow()
            expect(function()
                json.serialize({ run = function() end } :: any)
            end):ToThrow()
            expect(function()
                json.serialize(game:GetService("Workspace") :: any)
            end):ToThrow()
            -- A table that holds itself, which would be written for ever.
            local loop: any = {}
            loop.again = loop
            expect(function()
                json.serialize(loop)
            end):ToThrow()
        end)
    end)

    describe("deserialize", function()
        it("reads the plain values", function()
            expect(json.deserialize("true")):ToBe(true)
            expect(json.deserialize(" false ")):ToBe(false)
            expect(json.deserialize("null")):ToBe(json.null)
            expect(json.deserialize("-12.5e1")):ToBe(-125)
            expect(json.deserialize('"word"')):ToBe("word")
        end)

        it("reads escapes, and a character written as two halves", function()
            expect(json.deserialize('"a\\n\\t\\"\\\\\\/b"')):ToBe('a\n\t"\\/b')
            expect(json.deserialize('"\\u00e7\\u00e3o"')):ToBe("ção")
            -- U+1F600, as the pair JSON writes it.
            expect(json.deserialize('"\\ud83d\\ude00"')):ToBe("\u{1F600}")
        end)

        it("reads arrays and objects, however deep", function()
            local value: any = json.deserialize('{"name":"Ana","scores":[1,2,3],"pet":null,"nest":{"deep":[{"x":1}]}}')
            expect(value.name):ToBe("Ana")
            expect(#value.scores):ToBe(3)
            expect(value.scores[3]):ToBe(3)
            expect(value.pet):ToBe(json.null)
            expect(value.nest.deep[1].x):ToBe(1)
        end)

        it("gives an object its members and nothing else", function()
            local value: any = json.deserialize('{"a":1,"b":2}')
            local count = 0
            for key in value do
                expect(typeof(key)):ToBe("string")
                count += 1
            end
            expect(count):ToBe(2)
        end)

        it("tells an object from an array, the empty ones too", function()
            local object = json.deserialize("{}")
            local array = json.deserialize("[]")
            expect(json.asObject(object)):ToBe(object)
            expect(json.asArray(object)):ToBe(nil)
            expect(json.asArray(array)):ToBe(array)
            expect(json.asObject(array)):ToBe(nil)
            expect(json.asObject(5)):ToBe(nil)
            expect(json.asArray("text")):ToBe(nil)
            -- And writes each back as what it was.
            expect(json.serialize(object)):ToBe("{}")
            expect(json.serialize(array)):ToBe("[]")
        end)

        it("reads back what serialize wrote", function()
            local value: any = {
                name = "Hero",
                level = 12,
                alive = true,
                bag = { "sword", "bread", json.null, 3.25 } :: { any },
                stats = { strength = 7, luck = 0.125 },
            }
            local again: any = json.deserialize(json.serialize(value))
            expect(json.serialize(again)):ToBe(json.serialize(value))
            expect(again.bag[3]):ToBe(json.null)
            expect(again.stats.luck):ToBe(0.125)
        end)

        it("raises for text that is not JSON, and for anything after a value", function()
            for _, text in
                {
                    "",
                    "{",
                    "[1,]",
                    '{"a":}',
                    "{a:1}",
                    "01",
                    "1.",
                    "nul",
                    '"unclosed',
                    '"bad \\x escape"',
                    '"\\ud83d alone"',
                    "[1] 2",
                    "'single'",
                }
            do
                expect(function()
                    json.deserialize(text)
                end):ToThrow()
            end
            expect(function()
                json.deserialize(12 :: any)
            end):ToThrow()
        end)
    end)

    describe("object", function()
        it("is a copy that is an object, and leaves what it was given alone", function()
            local props: { [string]: any } = { a = 1 }
            local made = json.object(props)
            expect(made).Never:ToBe(props)
            expect(made.a):ToBe(1)
            expect(json.asObject(made)):ToBe(made)
            expect(json.asObject(props)):ToBe(nil)
        end)
    end)
end)
