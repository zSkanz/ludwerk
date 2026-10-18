--!strict
-- `@engine/xrgrab` (the polished VR work): a held thing is pulled to the
-- hand with a force of its own, so what it meets stops it.
--
--   - **The stack**: a block held over another resting on a table, set on
--     it and pushed ten centimetres further down. The lower block sinks no
--     more than five millimetres into the table, and the held block stays on
--     top of it, while the hand goes on below them.
--   - **The table**: a block held and pushed ten centimetres into the table
--     stops at its top.
--   - **The throw**: let go while the hand moves at two metres a second, the
--     block flies at the hand's speed.
local RunService = game:GetService("RunService")
local xrgrab = require("@engine/xrgrab")

local function part(size: vector, position: vector, anchored: boolean): Part
    local made = Instance.new("Part")
    made.Anchored = anchored
    made.Size = size
    made.Position = position
    made.Parent = workspace
    return made
end

local function fixed(value: number): string
    local text = string.format("%.1f", value)
    return if text == "-0.0" then "0.0" else text
end

local Half = 0.075
local TableTop = 0.8
part(vector.create(1.6, TableTop, 1), vector.create(0, TableTop / 2, 0), true)
local lower = part(vector.create(Half * 2, Half * 2, Half * 2), vector.create(-0.4, TableTop + Half, 0), false)
local upper = part(vector.create(Half * 2, Half * 2, Half * 2), vector.create(-0.4, TableTop + 0.4, 0), false)
local alone = part(vector.create(Half * 2, Half * 2, Half * 2), vector.create(0.4, TableTop + 0.4, 0), false)
local thrown = part(vector.create(Half * 2, Half * 2, Half * 2), vector.create(0, TableTop + 0.6, 0.3), false)

local ticks = 0
local stackHold: xrgrab.Hold? = nil
local aloneHold: xrgrab.Hold? = nil
local throwHold: xrgrab.Hold? = nil
local deepestLower = 0.0
local deepestAlone = 0.0
local upperOnTop = true
local handBelow = 0.0
local throwSpeed = 0.0
local Throw = 2.0

RunService.PreSimulation:Connect(function(dt: number)
    ticks += 1
    if ticks == 30 then
        stackHold = xrgrab.hold(upper, upper.CFrame)
        aloneHold = xrgrab.hold(alone, alone.CFrame)
        throwHold = xrgrab.hold(thrown, thrown.CFrame)
    end
    -- Down over a second, to ten centimetres below where each would rest.
    if ticks > 30 and ticks <= 150 then
        local t = math.min((ticks - 30) / 60, 1)
        local stackTarget = TableTop + Half * 3 - 0.1
        local aloneTarget = TableTop + Half - 0.1
        local stackY = (TableTop + 0.4) + (stackTarget - (TableTop + 0.4)) * t
        local aloneY = (TableTop + 0.4) + (aloneTarget - (TableTop + 0.4)) * t
        if stackHold then
            stackHold:Move(CFrame.new(-0.4, stackY, 0))
            handBelow = math.max(handBelow, upper.Position.y - stackY)
        end
        if aloneHold then
            aloneHold:Move(CFrame.new(0.4, aloneY, 0))
        end
    end
    -- The throw: the hand along +x at two metres a second, let go at tick 70.
    if throwHold and ticks > 30 and ticks <= 70 then
        local x = Throw * math.max(ticks - 40, 0) * dt
        throwHold:Move(CFrame.new(x, TableTop + 0.6, 0.3))
        if ticks == 70 then
            throwHold:Release()
            throwSpeed = thrown.LinearVelocity.x
        end
    end
end)

RunService.PostSimulation:Connect(function()
    if ticks > 90 and ticks <= 150 then
        deepestLower = math.max(deepestLower, TableTop - (lower.Position.y - Half))
        deepestAlone = math.max(deepestAlone, TableTop - (alone.Position.y - Half))
        upperOnTop = upperOnTop and upper.Position.y - Half >= lower.Position.y + Half - 0.005
    end
    if ticks == 150 then
        print(
            `[grab] lower sank {fixed(deepestLower * 1000)} mm, alone {fixed(deepestAlone * 1000)} mm, `
                .. `upper on top={upperOnTop}, hand below by {fixed(handBelow * 1000)} mm; `
                .. `thrown at {string.format("%.2f", throwSpeed)} m/s for a hand at {string.format("%.2f", Throw)}`
        )
        local stacked = deepestLower <= 0.005 and upperOnTop and handBelow >= 0.05
        local stopped = deepestAlone <= 0.005
        local thrownWell = math.abs(throwSpeed - Throw) <= 0.2 * Throw
        print(`[grab] ok={stacked and stopped and thrownWell}`)
    end
end)
