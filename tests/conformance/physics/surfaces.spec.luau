--!strict
--- Conformance: what a surface is to touch (ADR 0117).
---
--- A material says how it grips and how it bounces, what a footstep on it
--- sounds like and what it is tagged as; a part collides with what it wears,
--- a terrain with what each piece of ground is DRAWN as, and a ray and a
--- character say what they met. What is asserted here is each of those through
--- the tree and the real solver: a block slid across ice goes further than one
--- slid across grass, on parts and on terrain alike, and a rule that lays snow
--- over grass makes the ground snow to a ray, to a foot and to a sliding block.
---
--- The engine's own terrain materials are used throughout: `ice` grips a tenth
--- of what anything else does, `snow` half, and the rest are the 0.3 a body
--- has always met.
---
--- Everything waits in whole ticks through `task.wait`, because the tick is the
--- only clock a spec may read (ADR 0025).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local Ice = Material.load("engine://terrain/ice" :: Content)
local Snow = Material.load("engine://terrain/snow" :: Content)
local Grass = Material.load("engine://terrain/grass" :: Content)
local Brick = Material.load("asset://materials/brick.material.json" :: Content)

--- The layers the terrains below are given, by id: a new terrain has none,
--- and ground with no layer is made of nothing a script can name.
local Layers = { "engine://terrain/grass", "engine://terrain/snow", "engine://terrain/ice" }
local GrassId = 1
local SnowId = 2
local IceId = 3

local spawned: { Instance } = {}

local function cleanUp()
    for _, instance in spawned do
        instance:Destroy()
    end
    table.clear(spawned)
end

local function keep<T>(instance: T & Instance): T
    table.insert(spawned, instance)
    return instance
end

--- An anchored slab with its top at `top`, forty metres along X.
local function slab(at: vector, wears: Material?): Part
    local created = keep(Instance.new("Part"))
    created.Name = "Slab"
    created.Size = vector.create(40, 1, 6)
    created.Position = at - vector.create(0, 0.5, 0)
    created.Anchored = true
    created.Material = wears
    created.Parent = workspace
    return created
end

--- A block at rest on the ground at `at`, then sent along +X at six metres a
--- second: how far it got in two seconds.
local function slide(at: vector): number
    local block = Instance.new("Part")
    block.Name = "Block"
    block.Size = vector.create(1, 1, 1)
    block.Position = at + vector.create(0, 0.55, 0)
    block.Parent = workspace
    task.wait(0.5)
    local from = block.Position.x
    block.LinearVelocity = vector.create(6, 0, 0)
    task.wait(2)
    local went = block.Position.x - from
    -- Gone before the next one is sent down the same lane.
    block:Destroy()
    return went
end

describe("A material's surface", function()
    afterEach(cleanUp)

    it("says how it grips, how it bounces and what it is", function()
        expect(Ice.Friction):ToBeCloseTo(0.03, 1e-6)
        expect(Snow.Friction):ToBeCloseTo(0.15, 1e-6)
        expect(Grass.Friction):ToBeCloseTo(0.3, 1e-6)
        expect(Grass.Restitution):ToBe(0)
        expect(Ice.Tags[1]):ToBe("ice")
        -- A material that says nothing is what a body has always met.
        expect(Brick.Friction):ToBeCloseTo(0.3, 1e-6)
        expect(#Brick.Tags):ToBe(0)
    end)

    it("is a script's to change on a copy", function()
        local copy = Brick:Clone()
        copy.Friction = 0.9
        copy.Restitution = 0.5
        copy.Tags = { "wall", "loud" }
        expect(copy.Friction):ToBeCloseTo(0.9, 1e-6)
        expect(copy.Restitution):ToBeCloseTo(0.5, 1e-6)
        expect(copy.Tags[2]):ToBe("loud")
        expect(Brick.Friction):ToBeCloseTo(0.3, 1e-6)
        expect(function()
            (Brick :: any).Friction = 0.9
        end):ToThrow()
    end)
end)

describe("A part collides with what it wears", function()
    afterEach(cleanUp)

    it("a block slides further on ice than on grass", function()
        slab(vector.create(15, 10, 0), Ice)
        slab(vector.create(15, 10, 20), Grass)
        local onIce = slide(vector.create(0, 10, 0))
        local onGrass = slide(vector.create(0, 10, 20))
        -- Six metres on grass, where it stops; ten on ice, where it does not.
        expect(onGrass):ToBeCloseTo(6.1, 0.6)
        expect(onIce > onGrass + 2.5):ToBe(true)
    end)

    it("a part that says a friction of its own keeps it", function()
        local firm = slab(vector.create(15, 10, 0), Ice)
        firm.Friction = 1
        slab(vector.create(15, 10, 20), Ice)
        local onFirm = slide(vector.create(0, 10, 0))
        local onIce = slide(vector.create(0, 10, 20))
        expect(onIce > onFirm + 2.5):ToBe(true)
    end)

    it("a copy a script changes is felt the tick after", function()
        local copy = Ice:Clone()
        slab(vector.create(15, 10, 0), copy)
        local before = slide(vector.create(0, 10, 0))
        copy.Friction = 1
        local after = slide(vector.create(0, 10, 0))
        expect(before > after + 2.5):ToBe(true)
    end)

    it("a ray says what it met", function()
        slab(vector.create(0, 10, 0), Ice)
        slab(vector.create(0, 10, 20), nil)
        -- A part is a body the tick after it is in the world.
        task.wait()
        local hit = workspace:Raycast(vector.create(0, 14, 0), vector.create(0, -10, 0))
        expect(hit ~= nil):ToBe(true)
        assert(hit ~= nil)
        expect(hit.Material == Ice):ToBe(true)
        local material = hit.Material
        assert(material ~= nil)
        expect(material.Tags[1]):ToBe("ice")
        -- A part wearing nothing is made of nothing a script can name.
        local plain = workspace:Raycast(vector.create(0, 14, 20), vector.create(0, -10, 0))
        assert(plain ~= nil)
        expect(plain.Material):ToBe(nil)
    end)

    it("a character says what it stands on", function()
        slab(vector.create(0, 10, 0), Ice)
        local body = keep(Instance.new("CharacterBody"))
        body.Size = vector.create(2, 5, 2)
        body.Position = vector.create(0, 12.6, 0)
        body.Parent = workspace
        expect(body.FloorMaterial):ToBe(nil)
        task.wait(0.5)
        expect(body.Grounded):ToBe(true)
        expect(body.FloorMaterial == Ice):ToBe(true)
    end)
end)

describe("A terrain collides with what it is drawn as", function()
    local function ground(): Terrain
        local terrain = keep(Instance.new("Terrain"))
        terrain.Name = "SurfaceTerrain"
        terrain.MinHeight = -32
        terrain.MaxHeight = 32
        terrain.Parent = workspace
        terrain:SetLayers(Layers)
        return terrain
    end

    afterEach(cleanUp)

    it("a block slides further on its ice than on its grass", function()
        local terrain = ground()
        -- Two strips side by side, their tops at zero.
        terrain:FillBlock(vector.create(16, -8, 0), vector.create(64, 16, 12), GrassId)
        terrain:FillBlock(vector.create(16, -8, 24), vector.create(64, 16, 12), IceId)
        task.wait(0.5)
        local onGrass = slide(vector.create(0, 0, 0))
        local onIce = slide(vector.create(0, 0, 24))
        expect(onIce > onGrass + 2):ToBe(true)
    end)

    it("a ray says the layer the ground is, and the one a rule lays over it", function()
        local terrain = ground()
        terrain:FillBlock(vector.create(0, -8, 0), vector.create(32, 16, 32), GrassId)
        task.wait(0.2)
        local hit = workspace:Raycast(vector.create(0, 6, 0), vector.create(0, -12, 0))
        assert(hit ~= nil)
        expect(hit.Instance):ToBe(terrain)
        expect(hit.Material == Grass):ToBe(true)

        -- Snow over everything above the floor: the voxels are still grass,
        -- and the ground is snow.
        terrain:SetRules({ { Material = SnowId, HeightMin = -30 } })
        task.wait(0.2)
        local drawn = workspace:Raycast(vector.create(0, 6, 0), vector.create(0, -12, 0))
        assert(drawn ~= nil)
        expect(drawn.Material == Snow):ToBe(true)
        expect(terrain:ReadVoxels(vector.create(0.2, -1.8, 0.2), vector.create(0.8, -1.2, 0.8))[1]):ToBe(GrassId)
    end)

    it("a rule's snow is snow to a sliding block and to a foot", function()
        local terrain = ground()
        terrain:FillBlock(vector.create(16, -8, 0), vector.create(64, 16, 12), GrassId)
        task.wait(0.5)
        local onGrass = slide(vector.create(0, 0, 0))
        terrain:SetRules({ { Material = SnowId, HeightMin = -30 } })
        task.wait(0.5)
        local onSnow = slide(vector.create(0, 0, 0))
        expect(onSnow > onGrass + 1):ToBe(true)

        local body = keep(Instance.new("CharacterBody"))
        body.Size = vector.create(2, 5, 2)
        body.Position = vector.create(20, 2.6, 0)
        body.Parent = workspace
        task.wait(0.5)
        expect(body.Grounded):ToBe(true)
        expect(body.FloorMaterial == Snow):ToBe(true)
    end)
end)
