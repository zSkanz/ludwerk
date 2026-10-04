--!strict
--- Conformance: `ParticleEmitter` (F2), from a script.
---
--- **The particles themselves are not observable** -- they are a picture, and no
--- script can find one -- so what is pinned here is everything a script CAN
--- see: the defaults a new emitter has, what each property refuses, and that a
--- burst is a verb that works whether or not the emitter streams.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("ParticleEmitter", function()
    it("starts as a soft white stream that fades out as it dies", function()
        local emitter = Instance.new("ParticleEmitter")
        expect(emitter.Enabled):ToBe(true)
        expect(emitter.Rate):ToBe(20)
        expect(emitter.Lifetime):ToBe(2)
        expect(emitter.Speed):ToBe(4)
        expect(emitter.SpreadAngle):ToBe(15)
        expect(emitter.Acceleration):ToBe(vector.zero)
        expect(emitter.Drag):ToBe(0)
        expect(emitter.Color):ToBe(Color3.new(1, 1, 1))
        expect(emitter.ColorEnd):ToBe(Color3.new(1, 1, 1))
        expect(emitter.Size):ToBe(0.5)
        expect(emitter.SizeEnd):ToBe(0.5)
        expect(emitter.Transparency):ToBe(0)
        expect(emitter.TransparencyEnd):ToBe(1)
        expect(emitter.LightEmission):ToBe(0)
        expect(emitter.Brightness):ToBe(1)
        expect(emitter.Shape):ToBe(Enum.ParticleShape.Soft)
        emitter:Destroy()
    end)

    it("round-trips what it accepts", function()
        local emitter = Instance.new("ParticleEmitter")
        emitter.Rate = 250
        emitter.Acceleration = vector.create(0, -9.81, 0)
        emitter.ColorEnd = Color3.new(1, 0, 0)
        emitter.LightEmission = 1
        emitter.Shape = Enum.ParticleShape.Disc
        emitter.SpreadAngle = 180
        expect(emitter.Rate):ToBe(250)
        expect(emitter.Acceleration.y):ToBeCloseTo(-9.81, 1e-4)
        expect(emitter.ColorEnd):ToBe(Color3.new(1, 0, 0))
        expect(emitter.LightEmission):ToBe(1)
        expect(emitter.Shape):ToBe(Enum.ParticleShape.Disc)
        expect(emitter.SpreadAngle):ToBe(180)
        emitter:Destroy()
    end)

    it("refuses a negative rate, a fraction past one and a cone past a sphere", function()
        local emitter = Instance.new("ParticleEmitter") :: any
        expect(function()
            emitter.Rate = -1
        end):ToThrow()
        expect(function()
            emitter.Transparency = 1.5
        end):ToThrow()
        expect(function()
            emitter.LightEmission = 2
        end):ToThrow()
        expect(function()
            emitter.SpreadAngle = 181
        end):ToThrow()
        expect(function()
            emitter.Shape = Enum.PartShape.Block
        end):ToThrow()
        emitter:Destroy()
    end)

    it("emits a burst whether or not it streams", function()
        local part = Instance.new("Part")
        part.Parent = workspace
        local emitter = Instance.new("ParticleEmitter")
        emitter.Enabled = false
        emitter.Parent = part
        -- A verb, not a property: it returns nothing and raises nothing, and
        -- a count past the cap is clamped rather than refused.
        emitter:Emit(10)
        emitter:Emit(5000)
        emitter:Emit(0)
        part:Destroy()
    end)
end)

describe("ParticleEmitter's picture (ADR 0160)", function()
    it("takes a texture, a flipbook, a turn and curves over life, and starts with none of them", function()
        local emitter = Instance.new("ParticleEmitter")
        expect(emitter.Texture):ToBe("")
        expect(emitter.FlipbookColumns):ToBe(1)
        expect(emitter.FlipbookRows):ToBe(1)
        expect(emitter.FlipbookFramerate):ToBe(12)
        expect(emitter.FlipbookMode):ToBe(Enum.ParticleFlipbookMode.Loop)
        expect(emitter.Rotation):ToBe(0)
        expect(emitter.RotationSpread):ToBe(0)
        expect(emitter.RotationSpeed):ToBe(0)
        expect(emitter.RotationSpeedSpread):ToBe(0)
        expect(emitter.SizeOverLife.Keypoints[1].Value):ToBe(1)
        expect(emitter.TransparencyOverLife.Keypoints[1].Value):ToBe(0)

        emitter.Texture = "asset://textures/fire.png"
        emitter.FlipbookColumns = 4
        emitter.FlipbookRows = 4
        emitter.FlipbookMode = Enum.ParticleFlipbookMode.OverLife
        emitter.Rotation = 45
        emitter.RotationSpeed = -90
        emitter.SizeOverLife = NumberSequence.new(0, 2)
        emitter.ColorOverLife = ColorSequence.new(Color3.new(1, 1, 0), Color3.new(1, 0, 0))
        expect(emitter.Texture):ToBe("asset://textures/fire.png")
        expect(emitter.FlipbookColumns):ToBe(4)
        expect(emitter.FlipbookMode):ToBe(Enum.ParticleFlipbookMode.OverLife)
        expect(emitter.RotationSpeed):ToBe(-90)
        expect(emitter.SizeOverLife.Keypoints[2].Value):ToBe(2)
        emitter:Destroy()
    end)

    it("meets nothing unless told to, and bounces by default where it does", function()
        local emitter = Instance.new("ParticleEmitter")
        expect(emitter.Collision):ToBe(Enum.ParticleCollision.None)
        expect(emitter.CollisionResponse):ToBe(Enum.ParticleCollisionResponse.Bounce)
        expect(emitter.Bounce):ToBe(0.5)
        expect(emitter.CollisionRadius):ToBe(0)
        expect(emitter.Simulation):ToBe(Enum.ParticleSimulation.Cpu)
        emitter.Collision = Enum.ParticleCollision.Both
        emitter.CollisionResponse = Enum.ParticleCollisionResponse.Kill
        emitter.Simulation = Enum.ParticleSimulation.Gpu
        emitter.Bounce = 1
        expect(emitter.Collision):ToBe(Enum.ParticleCollision.Both)
        expect(emitter.Simulation):ToBe(Enum.ParticleSimulation.Gpu)
        expect(function()
            (emitter :: any).Bounce = 1.5
        end):ToThrow()
        expect(function()
            (emitter :: any).Friction = -0.1
        end):ToThrow()
        emitter:Destroy()
    end)

    it("refuses a flipbook past sixteen a side, a fraction of a frame, and a negative spread", function()
        local emitter = Instance.new("ParticleEmitter") :: any
        expect(function()
            emitter.FlipbookColumns = 17
        end):ToThrow()
        expect(function()
            emitter.FlipbookRows = 2.5
        end):ToThrow()
        expect(function()
            emitter.FlipbookColumns = 0
        end):ToThrow()
        expect(function()
            emitter.RotationSpread = -1
        end):ToThrow()
        expect(function()
            emitter.FlipbookFramerate = -1
        end):ToThrow()
        emitter:Destroy()
    end)
end)

describe("Decal's blend (ADR 0160)", function()
    it("multiplies unless told to lay over or add, and glows by its Emissive", function()
        local decal = Instance.new("Decal")
        expect(decal.BlendMode):ToBe(Enum.DecalBlendMode.Multiply)
        expect(decal.Emissive):ToBe(0)
        decal.BlendMode = Enum.DecalBlendMode.Additive
        decal.Emissive = 2
        expect(decal.BlendMode):ToBe(Enum.DecalBlendMode.Additive)
        expect(decal.Emissive):ToBe(2)
        expect(function()
            (decal :: any).Emissive = -1
        end):ToThrow()
        decal:Destroy()
    end)
end)

describe("Decal", function()
    it("is a box with an image, placed like a raycast hit", function()
        local decal = Instance.new("Decal")
        expect(decal.Size):ToBe(vector.create(2, 2, 1))
        expect(decal.Color):ToBe(Color3.new(1, 1, 1))
        expect(decal.Transparency):ToBe(0)
        decal.CFrame = CFrame.lookAt(vector.create(0, 0, 0), vector.create(0, 1, 0))
        decal.Texture = "asset://textures/mark.png"
        expect(decal.Texture):ToBe("asset://textures/mark.png")
        decal.Parent = workspace
        decal:Destroy()
    end)

    it("refuses a box with no extent and a transparency past one", function()
        local decal = Instance.new("Decal") :: any
        expect(function()
            decal.Size = vector.create(0, 2, 1)
        end):ToThrow()
        expect(function()
            decal.Transparency = 2
        end):ToThrow()
        decal:Destroy()
    end)
end)
