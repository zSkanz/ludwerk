--!strict
--[[
	The `project.toml` reader (M3 brief, Decision 9).

	The two properties worth testing are the ones a config format lives or dies
	on: it reads what api-design.md §4 says a project file holds, and it REFUSES
	anything it does not understand rather than ignoring it. A reader that
	silently skips a line is how a `[dev] port` ends up not applying, and that
	failure is invisible until someone wonders why the port is wrong.
]]

local assert = require("@std/test/assert")
local test = require("@std/test")
local toml = require("../toml")

test.suite("toml", function(suite)
    suite:case("reads the shape a project.toml actually has", function()
        local result = toml.parse([[
# A project file, as api-design.md §4 describes one.
[project]
name = "my-game"
id = "dev.example.my-game"
version = "0.1.0"
engine = "0.1"

[window]
title = "My Game"
size = [1280, 720]

[dev]
port = 4560

[permissions]
net_serve = false
fs_paths = ["save://", "asset://"]
]])

        assert.that(result.ok, result.diagnostic)
        assert.eq(result.table.project.name, "my-game")
        assert.eq(result.table.project.engine, "0.1")
        assert.eq(result.table.window.title, "My Game")
        assert.eq(result.table.window.size[1], 1280)
        assert.eq(result.table.window.size[2], 720)
        assert.eq(result.table.dev.port, 4560)
        assert.eq(result.table.permissions.net_serve, false)
        assert.eq(result.table.permissions.fs_paths[2], "asset://")
    end)

    suite:case("a CRLF line reads as the same line", function()
        local result = toml.parse('[export]\r\nmultiplayer = "dedicated"\r\ncount = 3\r\n')
        assert.that(result.ok, result.diagnostic)
        assert.eq(result.table.export.multiplayer, "dedicated")
        assert.eq(result.table.export.count, 3)
    end)

    suite:case("a comment is not a value, and a # inside a string is not a comment", function()
        local result = toml.parse([[
key = "a # b" # trailing
other = 'literal # too'
]])
        assert.that(result.ok, result.diagnostic)
        assert.eq(result.table.key, "a # b")
        assert.eq(result.table.other, "literal # too")
    end)

    suite:case("dotted headers and dotted keys reach the same place", function()
        local result = toml.parse([[
[a.b]
c = 1

[a]
d = 2
e.f = 3
]])
        assert.that(result.ok, result.diagnostic)
        assert.eq(result.table.a.b.c, 1)
        assert.eq(result.table.a.d, 2)
        assert.eq(result.table.a.e.f, 3)
    end)

    suite:case("numbers, booleans and escapes", function()
        local result = toml.parse([[
count = 1_000
ratio = -2.5
yes = true
no = false
path = "one\ttwo\nthree"
]])
        assert.that(result.ok, result.diagnostic)
        assert.eq(result.table.count, 1000)
        assert.eq(result.table.ratio, -2.5)
        assert.eq(result.table.yes, true)
        assert.eq(result.table.no, false)
        assert.eq(result.table.path, "one\ttwo\nthree")
    end)

    suite:case("an empty array and a trailing comma are both legal", function()
        local result = toml.parse([[
none = []
some = [1, 2, 3,]
]])
        assert.that(result.ok, result.diagnostic)
        assert.eq(#result.table.none, 0)
        assert.eq(#result.table.some, 3)
    end)

    suite:case("a file an editor on Windows marked as UTF-8 reads as the same file (D415)", function()
        -- Notepad and `Set-Content -Encoding utf8` put a byte-order mark first,
        -- and the first line then was "expected key = value".
        local result = toml.parse("\u{FEFF}[project]\nname = 'marked'" .. "\n")
        assert.that(result.ok, result.diagnostic)
        assert.eq(result.table.project.name, "marked")

        -- Only at the start: anywhere else it is somebody's text.
        local inside = toml.parse("[project]\n\u{FEFF}name = 'x'" .. "\n")
        assert.that(not inside.ok or inside.table.project.name == nil, "a mark in the middle of a file is not skipped")
    end)

    suite:case("what it does not support, it refuses by name and line", function()
        local cases = {
            { text = "[[products]]\nname = 'x'\n", contains = "arrays of tables" },
            { text = "key = { inline = true }\n", contains = "not a value" },
            { text = "no_equals_here\n", contains = "expected key = value" },
            { text = 'unterminated = "oops\n', contains = "unterminated" },
            { text = "[unclosed\n", contains = "unterminated table header" },
            { text = 'bad = "a" trailing\n', contains = "trailing text" },
        }

        for _, entry in cases do
            local result = toml.parse(entry.text)
            assert.that(not result.ok, `expected a refusal for: {entry.text}`)
            assert.strContains(result.diagnostic, entry.contains)
            assert.that(result.line > 0, "a diagnostic names the line it is about")
        end
    end)
end)
