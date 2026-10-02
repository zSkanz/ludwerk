--!strict
--- Conformance: a character that is pushed, floats, swims and flies (D466).
---
--- A block-world game found that `ApplyImpulse` on a `CharacterBody` did
--- nothing, that its velocity could not be written, that it had no gravity of
--- its own, and that swimming was `JumpSpeed = 0; Jump()` every tick. What is
--- pinned here: an impulse is a change of speed of the impulse over the mass,
--- and it fades; a velocity written is where the character is thrown;
--- `GravityScale` scales the fall; `Flying` takes its weight away; and a
--- character whose middle is under a `Water`'s surface is `Swimming`, by
--- itself, and leaves it by itself. (In a fluid block too: that case is with
--- the block world's own, `world/voxel.spec.luau`, whose block types it shares.)

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local PhysicsService = game:GetService("PhysicsService")

local spawned: { Instance } = {}

local function cleanUp()
    for _, instance in spawned do
        instance:Destroy()
    end
    table.clear(spawned)
end

local function ground(): Part
    local created = Instance.new("Part")
    created.Name = "Ground"
    created.Size = vector.create(200, 2, 200)
    created.Position = vector.create(0, -1, 0)
    created.Anchored = true
    created.Parent = workspace
    table.insert(spawned, created)
    return created
end

local function character(position: vector): CharacterBody
    local body = Instance.new("CharacterBody")
    body.Name = "Player"
    body.Size = vector.create(2, 5, 2)
    body.Position = position
    body.Parent = workspace
    table.insert(spawned, body)
    return body
end

local function ticks(seconds: number): number
    return math.floor(seconds / PhysicsService.FixedTimestep)
end

describe("A character that is pushed", function()
    afterEach(cleanUp)

    it("weighs what a controller weighs", function()
        local body = character(vector.create(0, 2.5, 0))
        task.wait()
        expect(body.Mass):ToBeCloseTo(80, 0.01)
    end)

    it("is knocked across the ground by an impulse, and the knock fades", function()
        ground()
        local body = character(vector.create(0, 2.5, 0))
        task.wait(0.3)
        -- Ten metres a second, sideways.
        body:ApplyImpulse(vector.create(body.Mass * 10, 0, 0))
        task.wait(0.1)
        expect(body.Position.x > 0.3):ToBe(true)
        task.wait(1.5)
        -- It stopped by itself, a metre or so on: not where ten metres a
        -- second for that long would have put it.
        expect(vector.magnitude(body.LinearVelocity) < 0.05):ToBe(true)
        expect(body.Position.x > 0.6):ToBe(true)
        expect(body.Position.x < 2):ToBe(true)
    end)

    it("is launched by an impulse upwards, and comes down", function()
        ground()
        local body = character(vector.create(0, 2.5, 0))
        task.wait(0.3)
        expect(body.Grounded):ToBe(true)
        body:ApplyImpulse(vector.create(0, body.Mass * 12, 0))
        local highest = 0
        for _ = 1, ticks(1) do
            task.wait()
            highest = math.max(highest, body.Position.y)
        end
        -- Twelve metres a second against gravity is a little over seven up.
        expect(highest > 8):ToBe(true)
        task.wait(2)
        expect(body.Grounded):ToBe(true)
    end)

    it("is thrown where a written velocity says", function()
        ground()
        local body = character(vector.create(0, 2.5, 0))
        task.wait(0.3)
        body.LinearVelocity = vector.create(6, 9, 0)
        task.wait(0.3)
        expect(body.Position.y > 4):ToBe(true)
        expect(body.Position.x > 1.2):ToBe(true)
    end)

    it("still walks while it is being pushed", function()
        ground()
        local body = character(vector.create(0, 2.5, 0))
        task.wait(0.3)
        body:ApplyImpulse(vector.create(body.Mass * 10, 0, 0))
        for _ = 1, ticks(0.5) do
            body:Move(vector.create(0, 0, -1))
            task.wait()
        end
        -- Sideways from the push, forwards from the walk.
        expect(body.Position.x > 0.5):ToBe(true)
        expect(body.Position.z < -5):ToBe(true)
    end)
end)

describe("A character made where another was just destroyed", function()
    afterEach(cleanUp)

    it("is not shoved by the one that is gone", function()
        -- D467: the dead one's body was still in the simulation when the new
        -- one was made, and a controller keeps the contacts it is made with.
        local first = character(vector.create(0, 30, 0))
        first.GravityScale = 0
        task.wait(0.2)
        -- Destroyed here, so it is not the clean-up's to destroy again.
        table.remove(spawned, table.find(spawned, first))
        first:Destroy()
        local second = character(vector.create(0, 30, 0))
        second.GravityScale = 0
        task.wait(0.2)
        expect(vector.magnitude(second.Position - vector.create(0, 30, 0)) < 0.001):ToBe(true)
        expect(second.Grounded):ToBe(false)
    end)
end)

describe("CharacterBody.GravityScale and JumpSpeed", function()
    afterEach(cleanUp)

    it("without gravity it stays where it is", function()
        local body = character(vector.create(0, 30, 0))
        body.GravityScale = 0
        task.wait(1)
        expect(body.Position.y):ToBeCloseTo(30, 0.01)
    end)

    it("at a quarter of gravity it falls a quarter as far", function()
        local heavy = character(vector.create(0, 60, 0))
        local light = character(vector.create(20, 60, 0))
        light.GravityScale = 0.25
        task.wait(1)
        local fell = 60 - heavy.Position.y
        expect(fell > 3):ToBe(true)
        expect(60 - light.Position.y):ToBeCloseTo(fell / 4, 0.2)
    end)

    it("takes a negative jump speed, which is a slam", function()
        local body = character(vector.create(0, 60, 0))
        body.GravityScale = 0
        body.JumpSpeed = -10
        body:Jump()
        task.wait(0.5)
        expect(body.Position.y):ToBeCloseTo(55, 0.3)
    end)
end)

describe("CharacterBody.Flying", function()
    afterEach(cleanUp)

    it("has no weight and says it is flying", function()
        local body = character(vector.create(0, 30, 0))
        body.Flying = true
        task.wait(1)
        expect(body.Position.y):ToBeCloseTo(30, 0.01)
        expect(body.State):ToBe(Enum.CharacterState.Flying)
    end)

    it("moves in three dimensions at its fly speed", function()
        local body = character(vector.create(0, 30, 0))
        body.Flying = true
        body.FlySpeed = 8
        for _ = 1, ticks(1) do
            body:Move(vector.create(0, 1, 0))
            task.wait()
        end
        expect(body.Position.y):ToBeCloseTo(38, 0.3)
        for _ = 1, ticks(1) do
            body:Move(vector.create(1, 0, 0))
            task.wait()
        end
        expect(body.Position.x):ToBeCloseTo(8, 0.3)
        expect(body.Position.y):ToBeCloseTo(38, 0.3)
    end)

    it("falls again when it stops flying", function()
        local body = character(vector.create(0, 30, 0))
        body.Flying = true
        task.wait(0.2)
        body.Flying = false
        task.wait(1)
        expect(body.Position.y < 27):ToBe(true)
        expect(body.State):ToBe(Enum.CharacterState.Airborne)
    end)
end)

describe("A character in water", function()
    afterEach(cleanUp)

    local function sea(level: number): Water
        local water = Instance.new("Water")
        water.SurfaceLevel = level
        water.Parent = workspace
        table.insert(spawned, water)
        return water
    end

    it("swims by itself under the surface, and does not sink", function()
        sea(20)
        local body = character(vector.create(0, 10, 0))
        task.wait(1)
        expect(body.State):ToBe(Enum.CharacterState.Swimming)
        expect(body.Position.y):ToBeCloseTo(10, 0.05)
    end)

    it("swims where it is told, up included", function()
        sea(40)
        local body = character(vector.create(0, 10, 0))
        body.SwimSpeed = 6
        for _ = 1, ticks(1) do
            body:Move(vector.create(0, 1, 0))
            task.wait()
        end
        expect(body.Position.y):ToBeCloseTo(16, 0.3)
        for _ = 1, ticks(1) do
            body:Move(vector.create(0, 0, -1))
            task.wait()
        end
        expect(body.Position.z):ToBeCloseTo(-6, 0.3)
    end)

    it("is airborne above the surface and swimming once it has fallen in", function()
        sea(20)
        local body = character(vector.create(0, 40, 0))
        task.wait(0.2)
        expect(body.State):ToBe(Enum.CharacterState.Airborne)
        task.wait(3)
        expect(body.State):ToBe(Enum.CharacterState.Swimming)
        -- It fell in and stopped: it did not go on to the bottom of the sea.
        expect(body.Position.y > 5):ToBe(true)
    end)

    it("kicks up out of the water with a jump", function()
        sea(20)
        local body = character(vector.create(0, 19, 0))
        task.wait(0.2)
        body:Jump()
        local highest = 0
        for _ = 1, ticks(1) do
            task.wait()
            highest = math.max(highest, body.Position.y)
        end
        -- Through the surface a metre above it, and on into the air.
        expect(highest > 20.8):ToBe(true)
    end)
end)

describe("@engine/camera.firstPerson", function()
    afterEach(function()
        cleanUp()
        workspace.CurrentCamera = nil
    end)

    it("sits at the subject's eyes and looks where it is turned", function()
        local camera = require("@engine/camera")
        local body = character(vector.create(3, 30, 4))
        body.GravityScale = 0
        local rig = camera.firstPerson({ Subject = body, Height = 1.5 })
        table.insert(spawned, rig.Camera)
        rig:Update(1 / 60)
        local eye = rig.Camera.CFrame.Position
        expect(eye.x):ToBeCloseTo(3, 0.001)
        expect(eye.y):ToBeCloseTo(31.5, 0.001)
        expect(eye.z):ToBeCloseTo(4, 0.001)
        -- Forwards is down -Z until it is turned.
        expect(rig.Camera.CFrame.LookVector.z):ToBeCloseTo(-1, 0.001)

        -- The pointer moved right and up: it looks right, and up.
        rig:Look(Vector2.new(100, -50))
        rig:Update(1 / 60)
        local look = rig.Camera.CFrame.LookVector
        expect(look.x > 0.3):ToBe(true)
        expect(look.y > 0.15):ToBe(true)
        expect(rig.Pitch > 0):ToBe(true)

        -- What to walk in: flat, and the way it faces.
        local forward, right = rig:Basis()
        expect(forward.y):ToBe(0)
        expect(forward.x > 0.3):ToBe(true)
        expect(vector.dot(forward, right)):ToBeCloseTo(0, 0.001)
    end)

    it("cannot be turned past straight up", function()
        local camera = require("@engine/camera")
        local body = character(vector.create(0, 30, 0))
        body.GravityScale = 0
        local rig = camera.firstPerson({ Subject = body })
        table.insert(spawned, rig.Camera)
        rig:Look(Vector2.new(0, -100000))
        rig:Update(1 / 60)
        expect(rig.Pitch < math.pi / 2):ToBe(true)
        expect(rig.Camera.CFrame.LookVector.y > 0.99):ToBe(true)
    end)
end)
