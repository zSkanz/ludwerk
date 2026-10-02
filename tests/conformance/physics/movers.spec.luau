--!strict
--- Conformance: movers, and what a joint does of itself (ADR 0127).
---
--- A fan, a lift, a rope, a pet that hovers at a shoulder: made of instances
--- and set in properties, with no script running each tick. What is asserted
--- here is that each of them does, through the tree and the real solver, what
--- its page says -- a motor reaches its speed and a weak one does not, a servo
--- settles at its angle, a pull holds a weight it is strong enough for and
--- sags under one it is not, a rope is never longer than its length.
---
--- Parts are two metres a side at a density of one: eight kilograms, so a
--- weight is `8 * g` newtons and a number in a case can be checked by hand.
---
--- Everything waits in whole ticks through `task.wait`, because the tick is the
--- only clock a spec may read (ADR 0025).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local spawned: { Instance } = {}
local Gravity = workspace.Gravity
local G = -Gravity.y

local function cleanUp()
    for _, instance in spawned do
        instance:Destroy()
    end
    table.clear(spawned)
    workspace.Gravity = Gravity
end

local function part(name: string, position: vector): Part
    local created = Instance.new("Part")
    created.Name = name
    created.Size = vector.create(2, 2, 2)
    created.Position = position
    created.Parent = workspace
    table.insert(spawned, created)
    return created
end

local function anchoredPart(name: string, position: vector): Part
    local created = part(name, position)
    created.Anchored = true
    return created
end

local function attachmentOn(host: BasePart, offset: CFrame?): Attachment
    local created = Instance.new("Attachment")
    if offset ~= nil then
        created.CFrame = offset
    end
    created.Parent = host
    table.insert(spawned, created)
    return created
end

local function keep<T>(instance: T & Instance): T
    table.insert(spawned, instance)
    return instance
end

local function span(a: Attachment, b: Attachment): number
    local difference = a.WorldCFrame.Position - b.WorldCFrame.Position
    return vector.magnitude(difference)
end

--- A joint frame's X is its axis. Turned a quarter about Y it points along the
--- world's Z, which is the axis gravity can turn an arm about.
local AlongZ = CFrame.fromEuler(0, -math.pi / 2, 0)
--- And turned a quarter about Z it points up: a lift's rail.
local Upwards = CFrame.fromEuler(0, 0, math.pi / 2)

--- An anchored post with an arm out along +X, hinged about Z where they meet.
local function hingedArm(): (HingeConstraint, Part)
    local post = anchoredPart("Post", vector.create(0, 20, 0))
    local arm = part("Arm", vector.create(3, 20, 0))
    local joint = Instance.new("HingeConstraint")
    joint.Attachment0 = attachmentOn(post, CFrame.new(1.5, 0, 0) * AlongZ)
    joint.Attachment1 = attachmentOn(arm, CFrame.new(-1.5, 0, 0) * AlongZ)
    joint.CollideConnected = false
    joint.Parent = workspace
    return keep(joint), arm
end

describe("HingeConstraint with an actuator", function()
    afterEach(cleanUp)

    it("a motor reaches its speed under its torque cap", function()
        local joint, arm = hingedArm()
        joint.ActuatorType = Enum.ActuatorType.Motor
        joint.AngularVelocity = 1
        joint.MotorMaxTorque = 100000
        task.wait(1)
        expect(arm.AngularVelocity.z):ToBeCloseTo(1, 0.1)
    end)

    it("a motor too weak for its load loses to gravity", function()
        -- Eight kilograms a metre and a half out is about 118 N m.
        local joint, arm = hingedArm()
        joint.ActuatorType = Enum.ActuatorType.Motor
        joint.AngularVelocity = 1
        joint.MotorMaxTorque = 5
        task.wait(0.4)
        expect(arm.AngularVelocity.z < 0):ToBe(true)
        expect(arm.Position.y < 20):ToBe(true)
    end)

    it("a motor winds up no faster than its acceleration", function()
        workspace.Gravity = vector.zero
        local joint, arm = hingedArm()
        joint.ActuatorType = Enum.ActuatorType.Motor
        joint.AngularVelocity = 4
        joint.MotorMaxTorque = 100000
        joint.MotorMaxAcceleration = 2
        task.wait(0.5)
        -- Half a second at two radians a second squared.
        expect(arm.AngularVelocity.z):ToBeCloseTo(1, 0.2)
    end)

    it("a servo settles at its angle and holds it against gravity", function()
        local joint, arm = hingedArm()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.TargetAngle = 45
        joint.AngularSpeed = 2
        joint.ServoMaxTorque = 100000
        task.wait(2.5)
        -- A metre and a half from the pivot, at forty-five degrees up.
        expect(arm.Position.y):ToBeCloseTo(20 + 1.5 * math.sin(math.rad(45)), 0.1)
        expect(arm.Position.x):ToBeCloseTo(1.5 + 1.5 * math.cos(math.rad(45)), 0.1)
        expect(vector.magnitude(arm.AngularVelocity) < 0.05):ToBe(true)
    end)

    it("a servo goes no faster than its speed", function()
        local joint, arm = hingedArm()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.TargetAngle = 90
        joint.AngularSpeed = 0.5
        joint.ServoMaxTorque = 100000
        task.wait(1)
        -- Half a radian in a second, not the quarter turn it was asked for.
        expect(arm.Position.y):ToBeCloseTo(20 + 1.5 * math.sin(0.5), 0.12)
    end)

    it("a servo with a stiffness is a spring to its angle", function()
        local joint, arm = hingedArm()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.TargetAngle = 30
        joint.ServoMaxTorque = 100000
        joint.Stiffness = 20000
        joint.Damping = 2000
        task.wait(3)
        expect(arm.Position.y):ToBeCloseTo(20 + 1.5 * math.sin(math.rad(30)), 0.15)
    end)
end)

describe("PrismaticConstraint", function()
    afterEach(cleanUp)

    local function lift(): (PrismaticConstraint, Part)
        local base = anchoredPart("Base", vector.create(0, 10, 0))
        local platform = part("Platform", vector.create(0, 12, 0))
        local joint = Instance.new("PrismaticConstraint")
        joint.Attachment0 = attachmentOn(base, CFrame.new(0, 1, 0) * Upwards)
        joint.Attachment1 = attachmentOn(platform, CFrame.new(0, -1, 0) * Upwards)
        joint.CollideConnected = false
        joint.Parent = workspace
        return keep(joint), platform
    end

    it("a servo raises a lift to its position and keeps it on its rail", function()
        local joint, platform = lift()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.TargetPosition = 3
        joint.Speed = 2
        joint.ServoMaxForce = 100000
        task.wait(3)
        expect(platform.Position.y):ToBeCloseTo(15, 0.1)
        expect(platform.Position.x):ToBeCloseTo(0, 0.05)
        expect(platform.Position.z):ToBeCloseTo(0, 0.05)
    end)

    it("a lift cannot raise more than its force", function()
        local joint, platform = lift()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.TargetPosition = 3
        joint.ServoMaxForce = 10
        task.wait(1)
        expect(platform.Position.y < 12):ToBe(true)
    end)

    it("a motor moves at its speed, and limits stop it", function()
        workspace.Gravity = vector.zero
        local joint, platform = lift()
        joint.ActuatorType = Enum.ActuatorType.Motor
        joint.Velocity = 2
        joint.MotorMaxForce = 100000
        joint.LimitsEnabled = true
        joint.LowerLimit = 0
        joint.UpperLimit = 1.5
        task.wait(0.5)
        expect(platform.LinearVelocity.y):ToBeCloseTo(2, 0.2)
        task.wait(1.5)
        expect(platform.Position.y):ToBeCloseTo(13.5, 0.1)
    end)
end)

describe("RopeConstraint and RodConstraint", function()
    afterEach(cleanUp)

    it("a rope is never longer than its length, past the tick it comes taut in", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(2, 29, 0))
        local top, eye = attachmentOn(anchor), attachmentOn(bob)
        local rope = keep(Instance.new("RopeConstraint"))
        rope.Attachment0 = top
        rope.Attachment1 = eye
        rope.Length = 5
        rope.Parent = workspace

        -- A limit is noticed once it is crossed, so the tick the rope comes
        -- taut in overshoots by what the bob travels in that tick -- here a
        -- tenth of a metre, at seven metres a second -- and the next one has
        -- it back. From then on it is its length, exactly.
        local longest, shortest = 0, math.huge
        for tick = 1, 150 do
            task.wait()
            longest = math.max(longest, span(top, eye))
            if tick > 70 then
                shortest = math.min(shortest, span(top, eye))
            end
        end
        expect(longest < 5.15):ToBe(true)
        expect(span(top, eye)):ToBeCloseTo(5, 0.01)
        -- And it is a rope, not a rod: it was slack before it was taut.
        expect(span(top, eye) > 2.5):ToBe(true)
        expect(shortest > 4.9):ToBe(true)
    end)

    it("a winch takes in rope at its speed, and lifts what hangs from it", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 25, 0))
        local rope = keep(Instance.new("RopeConstraint"))
        rope.Attachment0 = attachmentOn(anchor)
        rope.Attachment1 = attachmentOn(bob)
        rope.Length = 5
        rope.Parent = workspace
        task.wait(0.5)

        rope.WinchTarget = 2
        rope.WinchSpeed = 2
        rope.WinchForce = 100000
        rope.WinchEnabled = true
        task.wait(0.5)
        expect(rope.Length):ToBeCloseTo(4, 0.1)
        task.wait(2)
        expect(rope.Length):ToBeCloseTo(2, 0.01)
        expect(bob.Position.y):ToBeCloseTo(28, 0.15)
    end)

    it("a winch stalls on a load heavier than its force", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 25, 0))
        local rope = keep(Instance.new("RopeConstraint"))
        rope.Attachment0 = attachmentOn(anchor)
        rope.Attachment1 = attachmentOn(bob)
        rope.Length = 5
        rope.Parent = workspace
        task.wait(0.5)

        rope.WinchTarget = 2
        rope.WinchForce = 10
        rope.WinchEnabled = true
        task.wait(1)
        expect(rope.Length):ToBeCloseTo(5, 0.05)
    end)

    it("a rod holds its two ends exactly its length apart", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(4, 30, 0))
        local top, eye = attachmentOn(anchor), attachmentOn(bob)
        local rod = keep(Instance.new("RodConstraint"))
        rod.Attachment0 = top
        rod.Attachment1 = eye
        rod.Length = 4
        rod.Parent = workspace

        local shortest, longest = math.huge, 0
        for _ = 1, 120 do
            task.wait()
            shortest = math.min(shortest, span(top, eye))
            longest = math.max(longest, span(top, eye))
        end
        expect(longest < 4.1):ToBe(true)
        expect(shortest > 3.9):ToBe(true)
        expect(bob.Position.y < 29):ToBe(true)
    end)
end)

describe("SpringConstraint", function()
    afterEach(cleanUp)

    it("settles where its pull equals the weight", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 25, 0))
        local spring = keep(Instance.new("SpringConstraint"))
        spring.Attachment0 = attachmentOn(anchor)
        spring.Attachment1 = attachmentOn(bob)
        spring.FreeLength = 5
        spring.Stiffness = 200
        spring.Damping = 40
        spring.Parent = workspace
        task.wait(4)
        -- Stretched by the weight over the stiffness.
        expect(bob.Position.y):ToBeCloseTo(25 - 8 * G / 200, 0.1)
        expect(vector.magnitude(bob.LinearVelocity) < 0.1):ToBe(true)
    end)

    it("its limits are hard stops", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 25, 0))
        local top, eye = attachmentOn(anchor), attachmentOn(bob)
        local spring = keep(Instance.new("SpringConstraint"))
        spring.Attachment0 = top
        spring.Attachment1 = eye
        spring.FreeLength = 5
        spring.Stiffness = 1
        spring.Damping = 0
        spring.LimitsEnabled = true
        spring.MinLength = 4
        spring.MaxLength = 6
        spring.Parent = workspace
        task.wait(3)
        -- A spring this weak would let it fall for ever; the stop does not.
        expect(span(top, eye) < 6.1):ToBe(true)
    end)
end)

describe("LinearVelocity", function()
    afterEach(cleanUp)

    it("holds a velocity, gravity included", function()
        local crate = part("Crate", vector.create(0, 20, 0))
        local mover = keep(Instance.new("LinearVelocity"))
        mover.Attachment0 = attachmentOn(crate)
        mover.VectorVelocity = vector.create(2, 0, 0)
        mover.MaxForce = 100000
        mover.Parent = workspace
        task.wait(1)
        expect(crate.LinearVelocity.x):ToBeCloseTo(2, 0.05)
        expect(crate.Position.y):ToBeCloseTo(20, 0.05)
        expect(crate.Position.x):ToBeCloseTo(2, 0.15)
    end)

    it("in line mode holds the speed along the line and leaves the rest", function()
        local crate = part("Crate", vector.create(0, 40, 0))
        local mover = keep(Instance.new("LinearVelocity"))
        mover.Attachment0 = attachmentOn(crate)
        mover.VelocityConstraintMode = Enum.VelocityConstraintMode.Line
        mover.LineDirection = vector.create(1, 0, 0)
        mover.LineVelocity = 3
        mover.MaxForce = 100000
        mover.Parent = workspace
        task.wait(1)
        expect(crate.LinearVelocity.x):ToBeCloseTo(3, 0.05)
        -- It still falls.
        expect(crate.Position.y < 37):ToBe(true)
    end)

    it("a force too small for the weight does not hold it up", function()
        local crate = part("Crate", vector.create(0, 40, 0))
        local mover = keep(Instance.new("LinearVelocity"))
        mover.Attachment0 = attachmentOn(crate)
        mover.MaxForce = 4 * G
        mover.Parent = workspace
        task.wait(1)
        -- Half its weight: it falls at half of gravity.
        expect(crate.LinearVelocity.y):ToBeCloseTo(-G / 2, 0.6)
    end)

    it("follows its attachment's frame when told to", function()
        workspace.Gravity = vector.zero
        local crate = part("Crate", vector.create(0, 20, 0))
        crate.CFrame = CFrame.new(0, 20, 0) * CFrame.fromEuler(0, math.pi / 2, 0)
        local mover = keep(Instance.new("LinearVelocity"))
        mover.Attachment0 = attachmentOn(crate)
        mover.RelativeTo = Enum.ActuatorRelativeTo.Attachment0
        mover.VectorVelocity = vector.create(0, 0, -2)
        mover.MaxForce = 100000
        mover.Parent = workspace
        task.wait(0.5)
        -- "Forwards" for a part turned a quarter to its left is the world's -X.
        expect(crate.LinearVelocity.x):ToBeCloseTo(-2, 0.05)
        expect(crate.LinearVelocity.z):ToBeCloseTo(0, 0.05)
    end)
end)

describe("AngularVelocity", function()
    afterEach(cleanUp)

    it("holds a spin", function()
        workspace.Gravity = vector.zero
        local coin = part("Coin", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AngularVelocity"))
        mover.Attachment0 = attachmentOn(coin)
        mover.AngularVelocity = vector.create(0, 2, 0)
        mover.MaxTorque = 100000
        mover.Parent = workspace
        task.wait(0.5)
        expect(coin.AngularVelocity.y):ToBeCloseTo(2, 0.05)
    end)

    it("turns what it is mounted on the other way, when asked to", function()
        workspace.Gravity = vector.zero
        local rotor = part("Rotor", vector.create(0, 20, 0))
        local body = part("Body", vector.create(0, 24, 0))
        local mover = keep(Instance.new("AngularVelocity"))
        mover.Attachment0 = attachmentOn(rotor)
        mover.Attachment1 = attachmentOn(body)
        mover.AngularVelocity = vector.create(0, 2, 0)
        mover.MaxTorque = 20
        mover.ReactionTorqueEnabled = true
        mover.Parent = workspace
        task.wait(0.3)
        expect(rotor.AngularVelocity.y > 0.2):ToBe(true)
        expect(body.AngularVelocity.y < -0.2):ToBe(true)
    end)
end)

describe("AlignPosition", function()
    afterEach(cleanUp)

    it("holds against gravity within its force", function()
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Attachment0 = attachmentOn(pet)
        mover.Position = vector.create(0, 20, 0)
        mover.MaxForce = 100000
        mover.Parent = workspace
        task.wait(2)
        expect(pet.Position.y):ToBeCloseTo(20, 0.1)
    end)

    it("sags and falls past it", function()
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Attachment0 = attachmentOn(pet)
        mover.Position = vector.create(0, 20, 0)
        mover.MaxForce = 4 * G
        mover.Parent = workspace
        task.wait(2)
        expect(pet.Position.y < 15):ToBe(true)
    end)

    it("goes to a place no faster than its speed", function()
        workspace.Gravity = vector.zero
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Attachment0 = attachmentOn(pet)
        mover.Position = vector.create(10, 20, 0)
        mover.MaxForce = 100000
        mover.MaxVelocity = 2
        mover.Parent = workspace
        task.wait(1)
        expect(pet.Position.x):ToBeCloseTo(2, 0.2)
        task.wait(6)
        expect(pet.Position.x):ToBeCloseTo(10, 0.1)
    end)

    it("follows a second attachment", function()
        local owner = anchoredPart("Owner", vector.create(6, 20, 0))
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Mode = Enum.PositionAlignmentMode.TwoAttachment
        mover.Attachment0 = attachmentOn(pet)
        mover.Attachment1 = attachmentOn(owner, CFrame.new(0, 3, 0))
        mover.MaxForce = 100000
        mover.Parent = workspace
        task.wait(3)
        expect(pet.Position.x):ToBeCloseTo(6, 0.1)
        expect(pet.Position.y):ToBeCloseTo(23, 0.1)
    end)

    it("rigid, it is there at once", function()
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Attachment0 = attachmentOn(pet)
        mover.Position = vector.create(4, 22, 1)
        mover.RigidityEnabled = true
        mover.Parent = workspace
        task.wait(0.2)
        expect(vector.magnitude(pet.Position - vector.create(4, 22, 1)) < 0.05):ToBe(true)
    end)

    it("with a stiffness it is a spring: it sags by the weight over the stiffness", function()
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Attachment0 = attachmentOn(pet)
        mover.Position = vector.create(0, 20, 0)
        mover.MaxForce = 100000
        mover.Stiffness = 400
        mover.Damping = 80
        mover.Parent = workspace
        task.wait(4)
        expect(pet.Position.y):ToBeCloseTo(20 - 8 * G / 400, 0.05)
    end)

    it("pulls back on what it follows, when asked to", function()
        workspace.Gravity = vector.zero
        local hand = part("Hand", vector.create(0, 20, 0))
        local crate = part("Crate", vector.create(6, 20, 0))
        local mover = keep(Instance.new("AlignPosition"))
        mover.Mode = Enum.PositionAlignmentMode.TwoAttachment
        mover.Attachment0 = attachmentOn(crate)
        mover.Attachment1 = attachmentOn(hand)
        mover.MaxForce = 40
        mover.ReactionForceEnabled = true
        mover.Parent = workspace
        task.wait(0.5)
        -- Equal weights, equal and opposite pulls: they meet in the middle.
        expect(crate.LinearVelocity.x < -0.5):ToBe(true)
        expect(hand.LinearVelocity.x > 0.5):ToBe(true)
        expect(hand.LinearVelocity.x + crate.LinearVelocity.x):ToBeCloseTo(0, 0.05)
    end)
end)

describe("AlignOrientation", function()
    afterEach(cleanUp)

    it("turns to an orientation and stays there", function()
        workspace.Gravity = vector.zero
        local turret = part("Turret", vector.create(0, 20, 0))
        local target = CFrame.fromEuler(0, math.pi / 2, 0)
        local mover = keep(Instance.new("AlignOrientation"))
        mover.Attachment0 = attachmentOn(turret)
        mover.CFrame = target
        mover.MaxTorque = 100000
        mover.Parent = workspace
        task.wait(2)
        expect(vector.dot(turret.CFrame.LookVector, target.LookVector) > 0.999):ToBe(true)
        expect(vector.magnitude(turret.AngularVelocity) < 0.05):ToBe(true)
    end)

    it("turns no faster than its speed", function()
        workspace.Gravity = vector.zero
        local turret = part("Turret", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignOrientation"))
        mover.Attachment0 = attachmentOn(turret)
        mover.CFrame = CFrame.fromEuler(0, math.pi / 2, 0)
        mover.MaxTorque = 100000
        mover.MaxAngularVelocity = 0.5
        mover.Parent = workspace
        task.wait(0.5)
        expect(vector.magnitude(turret.AngularVelocity)):ToBeCloseTo(0.5, 0.05)
    end)

    it("faces what a second attachment faces", function()
        workspace.Gravity = vector.zero
        local owner = anchoredPart("Owner", vector.create(6, 20, 0))
        owner.CFrame = CFrame.new(6, 20, 0) * CFrame.fromEuler(0, 1, 0)
        local pet = part("Pet", vector.create(0, 20, 0))
        local mover = keep(Instance.new("AlignOrientation"))
        mover.Mode = Enum.OrientationAlignmentMode.TwoAttachment
        mover.Attachment0 = attachmentOn(pet)
        mover.Attachment1 = attachmentOn(owner)
        mover.MaxTorque = 100000
        mover.Parent = workspace
        task.wait(2)
        expect(vector.dot(pet.CFrame.LookVector, owner.CFrame.LookVector) > 0.999):ToBe(true)
    end)
end)

describe("VectorForce and Torque", function()
    afterEach(cleanUp)

    it("a force is a mass times how fast it speeds up", function()
        workspace.Gravity = vector.zero
        local crate = part("Crate", vector.create(0, 20, 0))
        crate.LinearDamping = 0
        local force = keep(Instance.new("VectorForce"))
        force.Attachment0 = attachmentOn(crate)
        force.RelativeTo = Enum.ActuatorRelativeTo.World
        force.Force = vector.create(16, 0, 0)
        force.ApplyAtCenterOfMass = true
        force.Parent = workspace
        task.wait(1)
        -- Sixteen newtons on eight kilograms for a second.
        expect(crate.LinearVelocity.x):ToBeCloseTo(2, 0.1)
    end)

    it("a force off the centre also turns the part", function()
        workspace.Gravity = vector.zero
        local crate = part("Crate", vector.create(0, 20, 0))
        local force = keep(Instance.new("VectorForce"))
        force.Attachment0 = attachmentOn(crate, CFrame.new(0, 1, 0))
        force.RelativeTo = Enum.ActuatorRelativeTo.World
        force.Force = vector.create(16, 0, 0)
        force.Parent = workspace
        task.wait(0.5)
        expect(math.abs(crate.AngularVelocity.z) > 0.5):ToBe(true)
    end)

    it("a torque is an inertia times how fast it spins up", function()
        workspace.Gravity = vector.zero
        local crate = part("Crate", vector.create(0, 20, 0))
        crate.AngularDamping = 0
        local torque = keep(Instance.new("Torque"))
        torque.Attachment0 = attachmentOn(crate)
        torque.RelativeTo = Enum.ActuatorRelativeTo.World
        -- A cube's inertia is its mass times its side squared over six.
        torque.Torque = vector.create(0, 8 * 4 / 6, 0)
        torque.Parent = workspace
        task.wait(1)
        expect(crate.AngularVelocity.y):ToBeCloseTo(1, 0.05)
    end)

    it("a disabled mover does nothing", function()
        workspace.Gravity = vector.zero
        local crate = part("Crate", vector.create(0, 20, 0))
        local force = keep(Instance.new("VectorForce"))
        force.Attachment0 = attachmentOn(crate)
        force.Force = vector.create(16, 0, 0)
        force.Enabled = false
        force.Parent = workspace
        task.wait(0.5)
        expect(vector.magnitude(crate.LinearVelocity)):ToBeCloseTo(0, 0.001)
    end)
end)

describe("BallSocketConstraint with a servo", function()
    afterEach(cleanUp)

    local function shoulder(): (BallSocketConstraint, Part)
        local chest = anchoredPart("Chest", vector.create(0, 20, 0))
        local arm = part("Arm", vector.create(3, 20, 0))
        local joint = Instance.new("BallSocketConstraint")
        joint.Attachment0 = attachmentOn(chest, CFrame.new(1.5, 0, 0))
        joint.Attachment1 = attachmentOn(arm, CFrame.new(-1.5, 0, 0))
        joint.CollideConnected = false
        joint.Parent = workspace
        return keep(joint), arm
    end

    it("limp, the arm falls", function()
        local _, arm = shoulder()
        task.wait(0.6)
        expect(arm.Position.y < 19.3):ToBe(true)
    end)

    it("driven, it holds the pose it was joined in", function()
        local joint, arm = shoulder()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.ServoMaxTorque = 100000
        task.wait(2)
        expect(arm.Position.y > 19.8):ToBe(true)
    end)

    it("driven to an orientation, it goes there", function()
        local joint, arm = shoulder()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.ServoMaxTorque = 100000
        -- The arm raised an eighth of a turn about Z.
        joint.TargetOrientation = CFrame.fromEuler(0, 0, math.pi / 4)
        task.wait(2.5)
        expect(arm.Position.y):ToBeCloseTo(20 + 1.5 * math.sin(math.pi / 4), 0.15)
    end)

    it("gives when pulled harder than its torque", function()
        local joint, arm = shoulder()
        joint.ActuatorType = Enum.ActuatorType.Servo
        joint.ServoMaxTorque = 20
        task.wait(1)
        expect(arm.Position.y < 19.3):ToBe(true)
    end)
end)

describe("NoCollisionConstraint", function()
    afterEach(cleanUp)

    it("lets one pair through and nothing else", function()
        local floor = anchoredPart("Floor", vector.create(0, 0, 0))
        floor.Size = vector.create(40, 2, 40)
        local ghost = part("Ghost", vector.create(-4, 4, 0))
        local crate = part("Crate", vector.create(4, 4, 0))
        local pair = keep(Instance.new("NoCollisionConstraint"))
        pair.Part0 = floor
        pair.Part1 = ghost
        pair.Parent = workspace
        task.wait(2)
        expect(ghost.Position.y < -5):ToBe(true)
        expect(crate.Position.y):ToBeCloseTo(2, 0.05)
    end)

    it("collides again when it is switched off", function()
        local floor = anchoredPart("Floor", vector.create(0, 0, 0))
        floor.Size = vector.create(40, 2, 40)
        local crate = part("Crate", vector.create(0, 6, 0))
        local pair = keep(Instance.new("NoCollisionConstraint"))
        pair.Part0 = floor
        pair.Part1 = crate
        pair.Enabled = false
        pair.Parent = workspace
        task.wait(2)
        expect(crate.Position.y):ToBeCloseTo(2, 0.05)
    end)
end)

describe("A constraint under load", function()
    afterEach(cleanUp)

    it("says the force it carries", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 26, 0))
        local rod = keep(Instance.new("RodConstraint"))
        rod.Attachment0 = attachmentOn(anchor)
        rod.Attachment1 = attachmentOn(bob)
        rod.Length = 4
        rod.Parent = workspace
        task.wait(1)
        -- Hanging still, it carries the weight.
        expect(rod:GetForce()):ToBeCloseTo(8 * G, 8)
        expect(rod:GetTorque()):ToBe(0)
    end)

    it("breaks past its force, says so, and holds nothing", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 26, 0))
        local rod = keep(Instance.new("RodConstraint"))
        rod.Attachment0 = attachmentOn(anchor)
        rod.Attachment1 = attachmentOn(bob)
        rod.Length = 4
        rod.BreakForce = 20
        rod.Parent = workspace

        local broke = 0
        rod.Broken:Connect(function()
            broke += 1
        end)
        task.wait(1.5)
        expect(broke):ToBe(1)
        expect(rod.Enabled):ToBe(false)
        expect(bob.Position.y < 22):ToBe(true)
    end)

    it("does not break under less", function()
        local anchor = anchoredPart("Anchor", vector.create(0, 30, 0))
        local bob = part("Bob", vector.create(0, 26, 0))
        local rod = keep(Instance.new("RodConstraint"))
        rod.Attachment0 = attachmentOn(anchor)
        rod.Attachment1 = attachmentOn(bob)
        rod.Length = 4
        rod.BreakForce = 1000
        rod.Parent = workspace
        task.wait(1.5)
        expect(rod.Enabled):ToBe(true)
        expect(bob.Position.y):ToBeCloseTo(26, 0.1)
    end)
end)

describe("What a part weighs and how it moves", function()
    afterEach(cleanUp)

    it("has the mass its size and density say, anchored or not", function()
        local crate = part("Crate", vector.create(0, 20, 0))
        local wall = anchoredPart("Wall", vector.create(10, 20, 0))
        wall.Density = 2
        task.wait()
        expect(crate.Mass):ToBeCloseTo(8, 0.01)
        expect(wall.Mass):ToBeCloseTo(16, 0.01)
    end)

    it("adds up everything welded to it", function()
        local base = anchoredPart("Base", vector.create(0, 20, 0))
        local top = part("Top", vector.create(0, 22, 0))
        local weld = keep(Instance.new("Weld"))
        weld.Part0 = base
        weld.Part1 = top
        weld.C0 = CFrame.new(0, 2, 0)
        weld.Parent = workspace
        local alone = part("Alone", vector.create(10, 20, 0))
        task.wait()
        expect(base.AssemblyMass):ToBeCloseTo(16, 0.01)
        expect(top.AssemblyMass):ToBeCloseTo(16, 0.01)
        expect(alone.AssemblyMass):ToBeCloseTo(8, 0.01)
    end)

    it("starts the next tick from a velocity a script wrote", function()
        workspace.Gravity = vector.zero
        local ball = part("Ball", vector.create(0, 20, 0))
        ball.LinearDamping = 0
        ball.AngularDamping = 0
        task.wait()
        ball.LinearVelocity = vector.create(10, 0, 0)
        ball.AngularVelocity = vector.create(0, 3, 0)
        -- Read back at once, before any tick.
        expect(ball.LinearVelocity.x):ToBe(10)
        task.wait(1)
        expect(ball.LinearVelocity.x):ToBeCloseTo(10, 0.01)
        expect(ball.AngularVelocity.y):ToBeCloseTo(3, 0.01)
        expect(ball.Position.x):ToBeCloseTo(10, 0.3)

        -- And stops dead when told to.
        ball.LinearVelocity = vector.zero
        task.wait(0.2)
        expect(vector.magnitude(ball.LinearVelocity)):ToBeCloseTo(0, 0.001)
    end)

    it("slows as its damping says", function()
        workspace.Gravity = vector.zero
        local free = part("Free", vector.create(0, 20, 0))
        local thick = part("Thick", vector.create(0, 30, 0))
        free.LinearDamping = 0
        thick.LinearDamping = 2
        task.wait()
        free.LinearVelocity = vector.create(10, 0, 0)
        thick.LinearVelocity = vector.create(10, 0, 0)
        task.wait(1)
        expect(free.LinearVelocity.x):ToBeCloseTo(10, 0.01)
        expect(thick.LinearVelocity.x < 2):ToBe(true)
        expect(thick.LinearVelocity.x > 0.5):ToBe(true)
    end)
end)

describe("BasePart.Collided", function()
    afterEach(cleanUp)

    it("says where, which way and how fast, for a part that asked", function()
        local floor = anchoredPart("Floor", vector.create(0, 0, 0))
        floor.Size = vector.create(40, 2, 40)
        -- Its underside five metres above the floor's top.
        local crate = part("Crate", vector.create(0, 7, 0))
        crate.ContactDetails = true

        local other: BasePart? = nil
        local where = vector.zero
        local way = vector.zero
        local fast = 0
        local times = 0
        crate.Collided:Connect(function(otherPart: BasePart, position: vector, normal: vector, speed: number)
            times += 1
            other, where, way, fast = otherPart, position, normal, speed
        end)
        task.wait(2)
        expect(times):ToBe(1)
        expect(other):ToBe(floor)
        expect(where.y):ToBeCloseTo(1, 0.3)
        -- From the crate into the floor.
        expect(way.y):ToBeCloseTo(-1, 0.01)
        expect(fast):ToBeCloseTo(math.sqrt(2 * G * 5), 0.6)
    end)

    it("stays silent for a part that did not ask", function()
        local floor = anchoredPart("Floor", vector.create(0, 0, 0))
        floor.Size = vector.create(40, 2, 40)
        local crate = part("Crate", vector.create(0, 4, 0))
        local times = 0
        crate.Collided:Connect(function()
            times += 1
        end)
        floor.Collided:Connect(function()
            times += 1
        end)
        task.wait(1.5)
        expect(times):ToBe(0)
    end)
end)

describe("Workspace:GetBodiesInSphere", function()
    afterEach(cleanUp)

    it("finds what a ball touches and nothing further", function()
        local near = anchoredPart("Near", vector.create(0, 50, 0))
        local edge = anchoredPart("Edge", vector.create(4, 50, 0))
        local far = anchoredPart("Far", vector.create(12, 50, 0))
        task.wait()
        local found = workspace:GetBodiesInSphere(vector.create(0, 50, 0), 3.5)
        expect(#found):ToBe(2)
        expect(table.find(found, near) ~= nil):ToBe(true)
        expect(table.find(found, edge) ~= nil):ToBe(true)
        expect(table.find(found, far)):ToBe(nil)

        local params = RaycastParams.new({ Filter = { near :: Instance } })
        expect(#workspace:GetBodiesInSphere(vector.create(0, 50, 0), 3.5, params)):ToBe(1)
    end)
end)
