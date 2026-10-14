--!strict
--[[
	A car, built the same on both ends of the gate: a chassis of default parts
	on four wheels, the front two steering and the rear two driven (ADR 0213).
]]

local Car = {}

--- A car at `at`, facing `facing` (-Z if not given), under `parent`, named
--- `name`.
function Car.build(name: string, at: vector, parent: Instance, facing: vector?): (Part, VehicleController)
    local chassis = Instance.new("Part")
    chassis.Name = name
    chassis.Size = vector.create(2, 0.6, 4)
    chassis.CFrame = CFrame.lookAt(at, at + (facing or vector.create(0, 0, -1)))
    chassis.Parent = parent
    local controller = Instance.new("VehicleController")
    controller.Parent = chassis
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
    end
    return chassis, controller
end

return Car
