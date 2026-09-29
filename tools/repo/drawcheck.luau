--!strict
--[[
	Drawn-position lint (ADR 0134).

	**Nothing visual reads a simulated transform.** Where a thing is drawn this
	frame is `render::DrawPoses`: between two ticks, and slid off after a
	replica's correction. The extraction drew parts there from M8, and the name
	over a character, its particles, the cameras of views, the 2D layer and the
	pointer's pick went on reading the simulated `cframe`, which moves a tick at
	a time. In a match the name smeared across the character it belonged to --
	the same defect, in six places, found one at a time.

	So the files that draw the world or put something over it may not read a
	`cframe`, a `worldCFrame` or a light's `partFrame` directly, nor call
	`interpolatedCFrame` -- which places a thing between ticks and leaves out a
	correction's slide. A read that is not a position being
	drawn -- a decal's own offset, an editor's camera that runs on the render
	clock, a cache key -- says so on its line or the line above it:

	    // raw: <why this is not a drawn position>

	Writes are not reads: `x.cframe = ...` and a designated `.cframe = ...` pass.

	Run from the repository root:

	    lute tools/repo/drawcheck.luau
]]

local fs = require("@std/fs")
local process = require("@std/process")

--- The files that draw the world, or draw or pick something placed in it.
local Visual = {
    "engine/render/src/render_world.cpp",
    "engine/render/src/particles.cpp",
    "engine/render/src/draw_poses.cpp",
    "engine/app/src/world_ui.cpp",
    "engine/app/src/view_host.cpp",
    "engine/app/src/picking.cpp",
    "engine/app/src/skeleton_overlay.cpp",
}

--- `DrawPoses` itself is where the simulated transform becomes a drawn one:
--- the one file allowed to read them all.
local Resolver = "engine/render/src/draw_poses.cpp"

local function isWrite(line: string, after: number): boolean
    local rest = string.sub(line, after)
    local spaced = string.match(rest, "^%s*=")
    return spaced ~= nil and string.match(rest, "^%s*==") == nil
end

local function exempt(lines: { string }, at: number): boolean
    if string.find(lines[at], "// raw:", 1, true) then
        return true
    end
    return at > 1 and string.find(lines[at - 1], "// raw:", 1, true) ~= nil
end

local problems: { string } = {}
for _, file in Visual do
    if file == Resolver then
        continue
    end
    if not fs.exists(file) then
        table.insert(problems, `{file}: listed here and missing -- a renamed file is a file this lint stopped reading`)
        continue
    end
    local lines: { string } = {}
    for line in string.gmatch(fs.readFileToString(file) .. "\n", "([^\n]*)\n") do
        table.insert(lines, line)
    end
    for index, line in lines do
        local code = string.gsub(line, "//.*$", "")
        for _, pattern in
            { "[%.>]cframe%f[^%w_]", "worldCFrame%f[^%w_]", "partFrame%f[^%w_]", "interpolatedCFrame%f[^%w_]" }
        do
            local start = 1
            while true do
                local first, last = string.find(code, pattern, start)
                if first == nil then
                    break
                end
                local finish = last :: number
                if not isWrite(code, finish + 1) and not exempt(lines, index) then
                    table.insert(
                        problems,
                        `{file}:{index}: reads a simulated transform; ask render::DrawPoses, or say why not with // raw:`
                    )
                end
                start = finish + 1
            end
        end
    end
end

if #problems > 0 then
    for _, problem in problems do
        print(`drawcheck: ERROR: {problem}`)
    end
    process.exit(1)
end
print(`drawcheck: ok -- {#Visual - 1} visual file(s) read every transform through DrawPoses`)
