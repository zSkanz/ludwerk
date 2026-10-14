--!strict
--- Conformance: a vehicle (ADR 0213).
---
--- A chassis part with a `VehicleController` and four `VehicleWheel`s, built
--- as a game builds one, driven through its properties and the real solver.
--- What is asserted is what each page says: it rests on its wheels, the
--- throttle sends it along its `LookVector`, the brake stops it, the steering
--- turns it, a manual box stays in the gear asked for -- and a value out of a
--- property's range is refused.
---
--- The chassis is default parts, a few kilograms: a torque or a brake left
--- at nought is the one for its mass, which is what makes this drive at all.
---
--- Everything waits in whole ticks through `task.wait` (ADR 0025).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local afterEach = testing.afterEach

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

local function ground(): Part
    local floor = keep(Instance.new("Part"))
    floor.Anchored = true
    floor.Size = vector.create(400, 1, 400)
    floor.Position = vector.create(0, -0.5, 0)
    floor.Parent = workspace
    return floor
end

-- A car on a floor: four wheels at its corners, the front two steering and
-- the rear two driven. Returned with its wheels, front left first.
local function car(): (Part, VehicleController, { VehicleWheel })
    ground()
    local chassis = keep(Instance.new("Part"))
    chassis.Size = vector.create(2, 0.6, 4)
    chassis.Position = vector.create(0, 1.2, 0)
    chassis.Parent = workspace
    local controller = Instance.new("VehicleController")
    controller.Parent = chassis
    local wheels: { VehicleWheel } = {}
    for _, corner in { { -0.9, -1.4 }, { 0.9, -1.4 }, { -0.9, 1.4 }, { 0.9, 1.4 } } do
        local front = corner[2] < 0
        local hub = Instance.new("Attachment")
        hub.CFrame = CFrame.new(corner[1], -0.2, corner[2])
        hub.Parent = chassis
        local wheel = Instance.new("VehicleWheel")
        wheel.Attachment = hub
        wheel.MaxSteerAngle = if front then 30 else 0
        wheel.Driven = not front
        wheel.Parent = controller
        table.insert(wheels, wheel)
    end
    return chassis, controller, wheels
end

describe("VehicleController and VehicleWheel", function()
    afterEach(cleanUp)

    it("start at their pages' defaults and refuse what is out of range", function()
        local controller = keep(Instance.new("VehicleController"))
        expect(controller.MaxTorque):ToBe(0)
        expect(controller.Transmission):ToBe(Enum.VehicleTransmission.Automatic)
        expect(controller.Throttle):ToBe(0)
        expect(controller.Gear):ToBe(1)
        expect(function()
            controller.Throttle = 2
        end):ToThrow()
        expect(function()
            controller.Brake = -0.5
        end):ToThrow()
        expect(function()
            controller.Gear = 1.5
        end):ToThrow()
        local wheel = keep(Instance.new("VehicleWheel"))
        expect(wheel.Radius):ToBeCloseTo(0.35, 0.0001)
        expect(wheel.Driven):ToBe(false)
        expect(wheel.Attachment):ToBe(nil)
        expect(function()
            wheel.Radius = 0
        end):ToThrow()
        expect(function()
            wheel.SuspensionDamping = 2
        end):ToThrow()
        -- A wheel's attachment is an attachment, and its part a part.
        local notAnAttachment = keep(Instance.new("Folder"))
        expect(function()
            wheel.Attachment = notAnAttachment :: any
        end):ToThrow()
    end)

    it("rests on its four wheels", function()
        local chassis, controller, wheels = car()
        task.wait(1)
        for _, wheel in wheels do
            expect(wheel.Grounded):ToBe(true)
            expect(wheel.Compression > 0 and wheel.Compression < 1):ToBe(true)
        end
        expect(math.abs(controller.Speed) < 0.1):ToBe(true)
        expect(chassis.Position.y > 0.5):ToBe(true)
    end)

    it("goes forward on the throttle, along its LookVector, and stops on the brake", function()
        local chassis, controller = car()
        task.wait(0.5)
        local start = chassis.Position
        controller.Throttle = 1
        task.wait(3)
        expect(controller.Speed > 3):ToBe(true)
        expect(controller.RPM > 0):ToBe(true)
        expect(controller.CurrentGear >= 1):ToBe(true)
        expect(chassis.Position.z < start.z - 5):ToBe(true)
        controller.Throttle = 0
        controller.Brake = 1
        task.wait(3)
        expect(math.abs(controller.Speed) < 0.3):ToBe(true)
    end)

    it("turns with the steering", function()
        local chassis, controller, wheels = car()
        task.wait(0.5)
        local start = chassis.Position
        controller.Throttle = 0.6
        controller.Steer = 1
        task.wait(3)
        -- Right, from facing -Z, is +X.
        expect(chassis.Position.x > start.x + 1):ToBe(true)
        expect(wheels[1].Slip ~= 0 or wheels[2].Slip ~= 0):ToBe(true)
    end)

    it("stays in the gear asked for with a manual gearbox, and goes back in reverse", function()
        local chassis, controller = car()
        task.wait(0.5)
        local start = chassis.Position
        controller.Transmission = Enum.VehicleTransmission.Manual
        controller.Gear = -1
        controller.Throttle = 1
        task.wait(2)
        expect(controller.CurrentGear):ToBe(-1)
        expect(controller.Speed < -0.5):ToBe(true)
        expect(chassis.Position.z > start.z + 1):ToBe(true)
    end)
end)
