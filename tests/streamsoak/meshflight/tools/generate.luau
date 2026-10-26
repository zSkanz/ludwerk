--!strict
--[[
	The mesh flight's world (ADR 0264), written into the folder it is given:

	    lute tests/streamsoak/meshflight/tools/generate.luau <project>

	A strip of cells, one after another along x, and in each a few dozen
	things wearing a mesh no other cell wears: what a flight along the strip
	loads, cell by cell, and what it must let go of behind it for the memory
	on the card to stay flat. The meshes are written too, as glTF files, each
	a grid bent by its own number so no two compile to the same content.
]]

local fs = require("@std/fs")
local process = require("@std/process")

local Root = process.args[2]
assert(Root ~= nil and Root ~= "", "generate.luau: the project folder is required")

local Cells = 48
local ChunkSize = 256
local Grid = 48
local ThingsPerCell = 24

local Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function base64(bytes: buffer): string
    local out: { string } = {}
    local length = buffer.len(bytes)
    for at = 0, length - 1, 3 do
        local a = buffer.readu8(bytes, at)
        local b = if at + 1 < length then buffer.readu8(bytes, at + 1) else 0
        local c = if at + 2 < length then buffer.readu8(bytes, at + 2) else 0
        local word = a * 65536 + b * 256 + c
        local first = bit32.rshift(word, 18)
        local second = bit32.band(bit32.rshift(word, 12), 63)
        local third = bit32.band(bit32.rshift(word, 6), 63)
        local fourth = bit32.band(word, 63)
        table.insert(out, string.sub(Alphabet, first + 1, first + 1))
        table.insert(out, string.sub(Alphabet, second + 1, second + 1))
        table.insert(out, if at + 1 < length then string.sub(Alphabet, third + 1, third + 1) else "=")
        table.insert(out, if at + 2 < length then string.sub(Alphabet, fourth + 1, fourth + 1) else "=")
    end
    return table.concat(out)
end

--- A grid of `Grid` by `Grid` vertices, a metre across, its heights a
--- ripple whose phase is `index`.
local function meshOf(index: number): string
    local vertices = Grid * Grid
    local quads = (Grid - 1) * (Grid - 1)
    local positionBytes = vertices * 12
    local indexBytes = quads * 6 * 4
    local bytes = buffer.create(positionBytes + indexBytes)
    local low = { math.huge, math.huge, math.huge }
    local high = { -math.huge, -math.huge, -math.huge }
    for row = 0, Grid - 1 do
        for column = 0, Grid - 1 do
            local x = column / (Grid - 1) - 0.5
            local z = row / (Grid - 1) - 0.5
            local y = 0.1 * math.sin(x * 9 + index) * math.cos(z * 7 + index * 0.37)
            local at = (row * Grid + column) * 12
            buffer.writef32(bytes, at, x)
            buffer.writef32(bytes, at + 4, y)
            buffer.writef32(bytes, at + 8, z)
            -- The bounds glTF asks for are the floats as stored.
            local stored = { buffer.readf32(bytes, at), buffer.readf32(bytes, at + 4), buffer.readf32(bytes, at + 8) }
            for axis = 1, 3 do
                low[axis] = math.min(low[axis], stored[axis])
                high[axis] = math.max(high[axis], stored[axis])
            end
        end
    end
    local at = positionBytes
    for row = 0, Grid - 2 do
        for column = 0, Grid - 2 do
            local a = row * Grid + column
            local b = a + 1
            local c = a + Grid
            local d = c + 1
            for _, corner in { a, c, b, b, c, d } do
                buffer.writeu32(bytes, at, corner)
                at += 4
            end
        end
    end
    return `\{"asset":\{"version":"2.0"\},"scene":0,"scenes":[\{"nodes":[0]\}],"nodes":[\{"mesh":0\}],`
        .. `"meshes":[\{"primitives":[\{"attributes":\{"POSITION":0\},"indices":1\}]\}],`
        .. `"accessors":[`
        .. `\{"bufferView":0,"componentType":5126,"count":{vertices},"type":"VEC3",`
        .. `"min":[{low[1]},{low[2]},{low[3]}],"max":[{high[1]},{high[2]},{high[3]}]\},`
        .. `\{"bufferView":1,"componentType":5125,"count":{quads * 6},"type":"SCALAR"\}],`
        .. `"bufferViews":[\{"buffer":0,"byteOffset":0,"byteLength":{positionBytes}\},`
        .. `\{"buffer":0,"byteOffset":{positionBytes},"byteLength":{indexBytes}\}],`
        .. `"buffers":[\{"byteLength":{positionBytes + indexBytes},`
        .. `"uri":"data:application/octet-stream;base64,{base64(bytes)}"\}]\}\n`
end

--- One cell: flat ground, and things wearing the cell's own mesh.
local function cellOf(cell: number): string
    local instances: { string } = {}
    local originX = cell * ChunkSize
    table.insert(
        instances,
        `\{"kind":"part","name":"Ground","anchored":true,"position":[{originX + ChunkSize / 2},-1,{ChunkSize / 2}],`
            .. `"size":[{ChunkSize},2,{ChunkSize}],"color":[0.3,0.4,0.25]\}`
    )
    for thing = 0, ThingsPerCell - 1 do
        local x = originX + 8 + (thing % 6) * 40
        local z = 20 + math.floor(thing / 6) * 50
        table.insert(
            instances,
            `\{"kind":"meshpart","name":"Thing","anchored":true,"mesh":"asset://models/m{cell}.gltf",`
                .. `"position":[{x},3,{z}],"size":[8,2,8],"color":[0.8,0.7,0.5]\}`
        )
    end
    return `\{"format":"chunk-source","chunkSize":{ChunkSize},"x":{cell},"z":0,"layer":0,`
        .. `"minY":-2,"maxY":8,"instances":[{table.concat(instances, ",")}]\}\n`
end

for _, folder in { `{Root}/content/models`, `{Root}/content/world` } do
    if not fs.exists(folder) then
        fs.createDirectory(folder, { makeParents = true })
    end
end
for cell = 0, Cells - 1 do
    fs.writeStringToFile(`{Root}/content/models/m{cell}.gltf`, meshOf(cell))
    fs.writeStringToFile(`{Root}/content/world/cell_{cell}_0.chunk.json`, cellOf(cell))
end
print(`meshflight: wrote {Cells} meshes and {Cells} cells to {Root}`)
