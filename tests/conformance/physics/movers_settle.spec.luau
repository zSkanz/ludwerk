--!strict
--- Conformance: a servo and a mover SETTLE (ADR 0127; D470, D471).
---
--- `movers.spec.luau` asks whether each of them gets where it is going. This
--- asks how: read tick by tick against the spring it says it is, on a body
--- nothing else holds and on bodies that joints hold together -- which is
--- where a mover worked out before the step, from the one body alone, fought
--- the joints and never came to rest.
---
--- Balls of forty centimetres, so what one weighs and what it is to turn can
--- be written by hand: `0.4 * mass * radius ^ 2` about its own middle.
---
--- Everything waits in whole ticks through `task.wait`, because the tick is the
--- only clock a spec may read (ADR 0025).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

local spawned: { Instance } = {}
local Gravity = workspace.Gravity
local G = -Gravity.y

local Radius = 0.2
local Volume = 4 / 3 * math.pi * Radius ^ 3
local Tick = 1 / 60

local function cleanUp()
    for _, instance in spawned do
        instance:Destroy()
    end
    table.clear(spawned)
    workspace.Gravity = Gravity
end

local function keep<T>(instance: T & Instance): T
    table.insert(spawned, instance)
    return instance
end

local function ball(at: vector, density: number, anchored: boolean?): Part
    local created = Instance.new("Part")
    created.Shape = Enum.PartShape.Ball
    created.Size = vector.create(Radius * 2, Radius * 2, Radius * 2)
    created.CFrame = CFrame.new(at)
    created.Density = density
    created.Anchored = anchored == true
    created.CanCollide = false
    -- Nothing but the thing under test takes speed away.
    created.LinearDamping = 0
    created.AngularDamping = 0
    created.Parent = workspace
    return keep(created)
end

local function attachmentAt(host: BasePart, world: CFrame): Attachment
    local created = Instance.new("Attachment")
    created.CFrame = host.CFrame:ToObjectSpace(world)
    created.Parent = host
    return keep(created)
end

--- A ball a metre below an anchored post, on a hinge about the world's X,
--- weighing what makes it `inertia` to turn about that hinge. The angle is by
--- the right hand about +X, zero hanging straight down.
local function pendulum(at: vector, inertia: number): (HingeConstraint, Part, () -> number)
    local mass = inertia / (1 + 0.4 * Radius ^ 2)
    local post = ball(at, 1000, true)
    local weight = ball(at - vector.create(0, 1, 0), mass / Volume)
    local hinge = keep(Instance.new("HingeConstraint"))
    hinge.Attachment0 = attachmentAt(post, CFrame.new(at))
    hinge.Attachment1 = attachmentAt(weight, CFrame.new(at))
    hinge.Parent = workspace
    return hinge,
        weight,
        function(): number
            local offset = weight.Position - at
            return math.deg(math.atan2(-offset.z, -offset.y))
        end
end

--- How far a part's up has turned from the world's, in degrees.
local function tilt(of: BasePart): number
    return math.deg(math.acos(math.clamp(of.CFrame.UpVector.y, -1, 1)))
end

--- Where a critically damped spring let go `step` from its target is after
--- `seconds`: the line everything below is read against.
local function ideal(step: number, rate: number, seconds: number): number
    return step * (1 - (1 + rate * seconds) * math.exp(-rate * seconds))
end

describe("A servo is a critically damped spring (D470)", function()
    afterEach(cleanUp)

    for index, inertia in { 0.01, 1, 100 } do
        it(`settles {inertia} kg m2 within a second and does not cross its angle`, function()
            workspace.Gravity = vector.zero
            local hinge, _, angle = pendulum(vector.create(index * 6, 30, 0), inertia)
            task.wait(0.1)
            hinge.ActuatorType = Enum.ActuatorType.Servo
            hinge.TargetAngle = 30
            local most = 0
            for _ = 1, 60 do
                task.wait()
                most = math.max(most, angle())
            end
            expect(most < 31):ToBe(true)
            expect(angle()):ToBeCloseTo(30, 1)
        end)
    end

    it("reads as the spring its responsiveness says, whatever the arm", function()
        -- A metre of arm is sixty times what the ball is to turn about its own
        -- middle: a spring sized for the middle rang for seconds.
        workspace.Gravity = vector.zero
        local hinge, _, angle = pendulum(vector.create(0, 30, 0), 1)
        task.wait(0.1)
        hinge.ActuatorType = Enum.ActuatorType.Servo
        hinge.ServoMaxTorque = 1e9
        hinge.AngularResponsiveness = 20
        hinge.TargetAngle = 30
        local worst = 0
        for sample = 1, 30 do
            task.wait()
            worst = math.max(worst, math.abs(angle() - ideal(30, 20, sample * Tick)))
        end
        expect(worst < 2):ToBe(true)
    end)

    it("too weak for its load, sags to an angle and rests there", function()
        -- A servo at its cap is a constant torque: it holds nothing back, so
        -- what brings the weight to rest is whatever else takes its speed --
        -- here the air, as a damping on the weight.
        local hinge, weight, angle = pendulum(vector.create(0, 30, 0), 1)
        weight.LinearDamping = 5
        hinge.ActuatorType = Enum.ActuatorType.Servo
        hinge.TargetAngle = 90
        -- Half of what the weight asks when the arm is level.
        hinge.ServoMaxTorque = weight.Mass * G * 0.5
        task.wait(3)
        local low, high = math.huge, -math.huge
        for _ = 1, 60 do
            task.wait()
            low = math.min(low, angle())
            high = math.max(high, angle())
        end
        expect(high - low < 0.5):ToBe(true)
        -- Where the weight's torque is the cap: the sine of thirty is a half,
        -- and a little past it for what the air takes of the fall.
        expect(angle() > 28 and angle() < 36):ToBe(true)
    end)

    it("strong enough for its load, holds its angle", function()
        local hinge, weight, angle = pendulum(vector.create(0, 30, 0), 1)
        hinge.ActuatorType = Enum.ActuatorType.Servo
        hinge.TargetAngle = 90
        hinge.ServoMaxTorque = weight.Mass * G * 2
        task.wait(2)
        for _ = 1, 30 do
            task.wait()
            expect(angle()):ToBeCloseTo(90, 2)
        end
    end)

    it("says what its motor is doing, apart from what the joint bears", function()
        local hinge, weight, angle = pendulum(vector.create(0, 30, 0), 1)
        hinge.ActuatorType = Enum.ActuatorType.Servo
        hinge.TargetAngle = 90
        hinge.ServoMaxTorque = 1e6
        task.wait(2)
        expect(angle()):ToBeCloseTo(90, 1)
        -- Level, the weight a metre out: its weight times that metre.
        expect(hinge:GetMotorTorque()):ToBeCloseTo(weight.Mass * G, weight.Mass * G * 0.05)
    end)
end)

describe("A mover is solved with the joints its body is in (D471)", function()
    afterEach(cleanUp)

    --- Pieces in a row along Y, each held to the next by a hinge with no
    --- range: one body in all but name, and every joint of it the solver's.
    local function chain(at: vector, count: number): { Part }
        local pieces: { Part } = {}
        for index = 1, count do
            pieces[index] = ball(at + vector.create(0, (index - 1) * 0.5, 0), 1000)
        end
        for index = 2, count do
            local below, above = pieces[index - 1], pieces[index]
            local middle = CFrame.new(below.Position + vector.create(0, 0.25, 0))
            local hinge = keep(Instance.new("HingeConstraint"))
            hinge.Attachment0 = attachmentAt(below, middle)
            hinge.Attachment1 = attachmentAt(above, middle)
            hinge.LimitsEnabled = true
            hinge.LowerAngle = 0
            hinge.UpperAngle = 0
            hinge.Parent = workspace
        end
        return pieces
    end

    --- What a row of `count` such balls is to turn about its own middle.
    local function chainInertia(count: number): number
        local mass = 1000 * Volume
        local total = 0
        for index = 1, count do
            local arm = (index - (count + 1) / 2) * 0.5
            total += 0.4 * mass * Radius ^ 2 + mass * arm ^ 2
        end
        return total
    end

    -- One, two and three: what a body with limbs is. A torque travels a joint
    -- a round of the solver, so a row of rigid joints turned from its middle
    -- lags at its ends: three pieces are a degree behind the line for a tick
    -- or two, five want four times the rounds a tick has, and that is the
    -- solver's to say, not a mover's.
    for _, count in { 1, 2, 3 } do
        it(`AlignOrientation turns {count} jointed pieces as the spring it was given`, function()
            workspace.Gravity = vector.zero
            local pieces = chain(vector.create(count * 8, 40, 0), count)
            local held = pieces[(count + 1) // 2]
            local inertia = chainInertia(count)
            local rate = 24
            local align = keep(Instance.new("AlignOrientation"))
            align.Mode = Enum.OrientationAlignmentMode.OneAttachment
            align.Attachment0 = attachmentAt(held, held.CFrame)
            align.MaxTorque = 1e9
            align.Stiffness = rate * rate * inertia
            align.Damping = 2 * rate * inertia
            align.CFrame = CFrame.identity
            align.Parent = workspace
            task.wait(0.2)
            align.CFrame = CFrame.fromAxisAngle(vector.create(1, 0, 0), math.rad(30))
            local worst = 0
            for sample = 1, 40 do
                task.wait()
                worst = math.max(worst, math.abs(tilt(held) - ideal(30, rate, sample * Tick)))
            end
            expect(worst < (if count == 3 then 3 else 2)):ToBe(true)
            -- And the far end went with it: one piece, not three.
            expect(tilt(pieces[count])):ToBeCloseTo(30, 1)
        end)
    end

    it("AlignPosition lifts a hanging rope by its end and does not overshoot", function()
        local at = vector.create(0, 40, 0)
        local pieces: { Part } = {}
        for index = 1, 10 do
            pieces[index] = ball(at - vector.create(0, (index - 1) * 0.5, 0), 1000)
        end
        for index = 2, 10 do
            local above, below = pieces[index - 1], pieces[index]
            local middle = CFrame.new(above.Position - vector.create(0, 0.25, 0))
            local socket = keep(Instance.new("BallSocketConstraint"))
            socket.Attachment0 = attachmentAt(above, middle)
            socket.Attachment1 = attachmentAt(below, middle)
            socket.Parent = workspace
        end
        local whole = 10 * 1000 * Volume
        local rate = 12
        local align = keep(Instance.new("AlignPosition"))
        align.Attachment0 = attachmentAt(pieces[1], pieces[1].CFrame)
        align.MaxForce = 1e9
        -- Critical for all that hangs from it.
        align.Stiffness = rate * rate * whole
        align.Damping = 2 * rate * whole
        align.Position = at
        align.Parent = workspace
        task.wait(1.5)
        -- What it settles under: the weight over the stiffness.
        local sag = G / (rate * rate)
        expect(pieces[1].Position.y):ToBeCloseTo(at.y - sag, 0.02)

        align.Position = at + vector.create(0, 2, 0)
        local highest = -math.huge
        for _ = 1, 90 do
            task.wait()
            highest = math.max(highest, pieces[1].Position.y)
        end
        expect(highest < at.y + 2 - sag + 0.02):ToBe(true)
        expect(pieces[1].Position.y):ToBeCloseTo(at.y + 2 - sag, 0.05)
        -- The rope came with it, still a rope.
        expect(pieces[10].Position.y):ToBeCloseTo(at.y + 2 - sag - 4.5, 0.1)
    end)

    it("a spring a hundred times stiffer than a tick can show does not explode", function()
        workspace.Gravity = vector.zero
        local at = vector.create(0, 40, 0)
        local held = ball(at, 1000)
        local mass = 1000 * Volume
        local inertia = 0.4 * mass * Radius ^ 2
        local place = keep(Instance.new("AlignPosition"))
        place.Attachment0 = attachmentAt(held, held.CFrame)
        place.MaxForce = 1e12
        place.Stiffness = 100 * mass / (Tick * Tick)
        place.Position = at + vector.create(1, 0, 0)
        place.Parent = workspace
        local face = keep(Instance.new("AlignOrientation"))
        face.Attachment0 = attachmentAt(held, held.CFrame)
        face.MaxTorque = 1e12
        face.Stiffness = 100 * inertia / (Tick * Tick)
        face.CFrame = CFrame.fromAxisAngle(vector.create(1, 0, 0), math.rad(60))
        face.Parent = workspace
        for _ = 1, 120 do
            task.wait()
            -- Never further from where it was sent than it started.
            expect(vector.magnitude(held.Position - place.Position) < 1.01):ToBe(true)
            expect(tilt(held) < 121):ToBe(true)
        end
    end)

    it("LinearVelocity on one of two bodies a rod joins moves both", function()
        workspace.Gravity = vector.zero
        local at = vector.create(0, 40, 0)
        local led = ball(at, 1000)
        local towed = ball(at - vector.create(2, 0, 0), 1000)
        local rod = keep(Instance.new("RodConstraint"))
        rod.Attachment0 = attachmentAt(led, led.CFrame)
        rod.Attachment1 = attachmentAt(towed, towed.CFrame)
        rod.Length = 2
        rod.Parent = workspace
        local mover = keep(Instance.new("LinearVelocity"))
        mover.Attachment0 = attachmentAt(led, led.CFrame)
        mover.VectorVelocity = vector.create(3, 0, 0)
        mover.MaxForce = 1e6
        mover.Parent = workspace
        task.wait(1)
        expect(led.LinearVelocity.x):ToBeCloseTo(3, 0.05)
        expect(towed.LinearVelocity.x):ToBeCloseTo(3, 0.1)
        expect(vector.magnitude(led.Position - towed.Position)):ToBeCloseTo(2, 0.02)
    end)

    it("AngularVelocity on a hinged door turns it about its hinge", function()
        workspace.Gravity = vector.zero
        local at = vector.create(0, 40, 0)
        local hinge, door, angle = pendulum(at, 1)
        local mover = keep(Instance.new("AngularVelocity"))
        mover.Attachment0 = attachmentAt(door, door.CFrame)
        mover.AngularVelocity = vector.create(2, 0, 0)
        mover.MaxTorque = 1e6
        mover.Parent = workspace
        task.wait(0.5)
        expect(door.AngularVelocity.x):ToBeCloseTo(2, 0.05)
        -- Still a metre from the pivot: the hinge was not fought.
        expect(vector.magnitude(door.Position - at)):ToBeCloseTo(1, 0.01)
        expect(hinge.Parent):ToBe(workspace)
        expect(angle() ~= 0):ToBe(true)
    end)
end)
