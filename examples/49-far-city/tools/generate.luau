--!strict
--[[
	Writes this example's world (ADRs 0261 to 0264) into the folder it is
	given, which is this example's own when none is:

	    lute examples/49-far-city/tools/generate.luau [<project>]

	Generated, not committed, as `05-streaming`'s is: megabytes of pictures
	and meshes nobody can diff. What is committed is this file.

	  - `content/models/tower_N.gltf`: a tower for each cell of the avenue, no
	    two alike -- a stack of rings, each bent by the tower's own number --
	    so each cell brings a mesh in and, left behind, lets one go.
	  - `content/world/cell_N_0.chunk.json`: a strip of cells along x, each a
	    stretch of street and ground with its towers back from the road.
	  - `content/textures/facade_N.tga` and `content/materials/facade_N.material.json`:
	    sixteen facades of windows, worn by the old town's blocks the script
	    builds at the avenue's start -- what the texture budget keeps smaller
	    as the town falls behind.
]]

local fs = require("@std/fs")
local process = require("@std/process")

local Root = process.args[2] or "examples/49-far-city"

local Cells = 40
local ChunkSize = 256
local TowersPerCell = 12
local Rings = 24
local Sides = 20
local Facades = 16
local FacadeSize = 512

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

--- A tower a metre across and a metre tall: rings of `Sides` vertices from
--- bottom to top, each pulled in and out by the tower's own number, capped.
local function towerOf(index: number): string
    local vertices = Rings * Sides + 1
    local triangles = (Rings - 1) * Sides * 2 + Sides
    local positionBytes = vertices * 12
    local indexBytes = triangles * 3 * 4
    local bytes = buffer.create(positionBytes + indexBytes)
    local low = { math.huge, math.huge, math.huge }
    local high = { -math.huge, -math.huge, -math.huge }
    local function put(at: number, x: number, y: number, z: number)
        buffer.writef32(bytes, at * 12, x)
        buffer.writef32(bytes, at * 12 + 4, y)
        buffer.writef32(bytes, at * 12 + 8, z)
        local stored =
            { buffer.readf32(bytes, at * 12), buffer.readf32(bytes, at * 12 + 4), buffer.readf32(bytes, at * 12 + 8) }
        for axis = 1, 3 do
            low[axis] = math.min(low[axis], stored[axis])
            high[axis] = math.max(high[axis], stored[axis])
        end
    end
    for ring = 0, Rings - 1 do
        local y = ring / (Rings - 1) - 0.5
        local radius = 0.42 + 0.06 * math.sin(ring * 0.9 + index * 1.7) - 0.1 * (y + 0.5)
        for side = 0, Sides - 1 do
            local angle = side / Sides * math.pi * 2
            local bend = 1 + 0.08 * math.cos(angle * 3 + index)
            put(ring * Sides + side, math.cos(angle) * radius * bend, y, math.sin(angle) * radius * bend)
        end
    end
    put(Rings * Sides, 0, 0.5, 0)
    local at = positionBytes
    local function triangle(a: number, b: number, c: number)
        buffer.writeu32(bytes, at, a)
        buffer.writeu32(bytes, at + 4, b)
        buffer.writeu32(bytes, at + 8, c)
        at += 12
    end
    for ring = 0, Rings - 2 do
        for side = 0, Sides - 1 do
            local a = ring * Sides + side
            local b = ring * Sides + (side + 1) % Sides
            local c = a + Sides
            local d = b + Sides
            triangle(a, c, b)
            triangle(b, c, d)
        end
    end
    local top = (Rings - 1) * Sides
    for side = 0, Sides - 1 do
        triangle(top + side, Rings * Sides, top + (side + 1) % Sides)
    end
    return `\{"asset":\{"version":"2.0"\},"scene":0,"scenes":[\{"nodes":[0]\}],"nodes":[\{"mesh":0\}],`
        .. `"meshes":[\{"primitives":[\{"attributes":\{"POSITION":0\},"indices":1\}]\}],`
        .. `"accessors":[`
        .. `\{"bufferView":0,"componentType":5126,"count":{vertices},"type":"VEC3",`
        .. `"min":[{low[1]},{low[2]},{low[3]}],"max":[{high[1]},{high[2]},{high[3]}]\},`
        .. `\{"bufferView":1,"componentType":5125,"count":{triangles * 3},"type":"SCALAR"\}],`
        .. `"bufferViews":[\{"buffer":0,"byteOffset":0,"byteLength":{positionBytes}\},`
        .. `\{"buffer":0,"byteOffset":{positionBytes},"byteLength":{indexBytes}\}],`
        .. `"buffers":[\{"byteLength":{positionBytes + indexBytes},`
        .. `"uri":"data:application/octet-stream;base64,{base64(bytes)}"\}]\}\n`
end

--- One cell: its stretch of ground, the road, and towers back from it on
--- either side, wearing the cell's own tower.
local function cellOf(cell: number): string
    local instances: { string } = {}
    local originX = cell * ChunkSize
    table.insert(
        instances,
        `\{"kind":"part","name":"Ground","anchored":true,"position":[{originX + ChunkSize / 2},-1,{ChunkSize / 2}],`
            .. `"size":[{ChunkSize},2,{ChunkSize}],"color":[0.32,0.4,0.28]\}`
    )
    table.insert(
        instances,
        `\{"kind":"part","name":"Road","anchored":true,"position":[{originX + ChunkSize / 2},0.05,128],`
            .. `"size":[{ChunkSize},0.1,14],"color":[0.18,0.18,0.2]\}`
    )
    for tower = 0, TowersPerCell - 1 do
        local x = originX + 12 + (tower % 6) * 42
        local side = if tower < 6 then -1 else 1
        local height = 30 + ((cell * 7 + tower * 13) % 50)
        table.insert(
            instances,
            `\{"kind":"meshpart","name":"Tower","anchored":true,"mesh":"asset://models/tower_{cell}.gltf",`
                .. `"position":[{x},{height / 2},{128 + side * 62}],"size":[16,{height},16],`
                .. `"color":[{0.55 + (tower % 3) * 0.1},{0.6},{0.68}]\}`
        )
    end
    return `\{"format":"chunk-source","chunkSize":{ChunkSize},"x":{cell},"z":0,"layer":0,`
        .. `"minY":-2,"maxY":90,"instances":[{table.concat(instances, ",")}]\}\n`
end

--- A facade: a wall of one colour with a grid of windows, uncompressed TGA.
local function facadeOf(index: number): buffer
    local size = FacadeSize
    local bytes = buffer.create(18 + size * size * 4)
    buffer.writeu8(bytes, 2, 2)
    buffer.writeu16(bytes, 12, size)
    buffer.writeu16(bytes, 14, size)
    buffer.writeu8(bytes, 16, 32)
    buffer.writeu8(bytes, 17, 0x28)
    local hue = index / Facades
    local wall = {
        math.floor(150 + 80 * math.sin(hue * 6.28)),
        math.floor(130 + 60 * math.sin(hue * 6.28 + 2.1)),
        math.floor(120 + 50 * math.sin(hue * 6.28 + 4.2)),
    }
    for y = 0, size - 1 do
        for x = 0, size - 1 do
            local window = (x % 64) > 14 and (x % 64) < 50 and (y % 64) > 12 and (y % 64) < 52
            local lit = window and ((math.floor(x / 64) * 7 + math.floor(y / 64) * 3 + index) % 5 == 0)
            local r, g, b = wall[1], wall[2], wall[3]
            if window then
                r, g, b = if lit then 230 else 40, if lit then 210 else 60, if lit then 140 else 80
            end
            local at = 18 + (y * size + x) * 4
            buffer.writeu8(bytes, at, b)
            buffer.writeu8(bytes, at + 1, g)
            buffer.writeu8(bytes, at + 2, r)
            buffer.writeu8(bytes, at + 3, 255)
        end
    end
    return bytes
end

local function facadeMaterial(index: number): string
    return `\{"format":"material","version":1,"parent":"","instanceParameters":["Color"],"properties":\{`
        .. `"Color":[1,1,1],"Transparency":0,"ColorMap":"asset://textures/facade_{index}.tga","NormalMap":"",`
        .. `"MetallicRoughnessMap":"","Emissive":[0,0,0],"EmissiveMap":"","Metalness":0,"Roughness":0.85,`
        .. `"NormalScale":1,"AlphaMode":"Opaque","AlphaCutoff":0.5,"DoubleSided":false\}\}\n`
end

for _, folder in { "content/models", "content/world", "content/textures", "content/materials" } do
    local path = `{Root}/{folder}`
    if not fs.exists(path) then
        fs.createDirectory(path, { makeParents = true })
    end
end
for cell = 0, Cells - 1 do
    fs.writeStringToFile(`{Root}/content/models/tower_{cell}.gltf`, towerOf(cell))
    fs.writeStringToFile(`{Root}/content/world/cell_{cell}_0.chunk.json`, cellOf(cell))
end
for index = 0, Facades - 1 do
    fs.writeStringToFile(`{Root}/content/textures/facade_{index}.tga`, buffer.tostring(facadeOf(index)))
    fs.writeStringToFile(`{Root}/content/materials/facade_{index}.material.json`, facadeMaterial(index))
end
print(`far-city: wrote {Cells} towers and cells and {Facades} facades to {Root}`)
