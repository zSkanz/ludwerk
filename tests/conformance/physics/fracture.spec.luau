--!strict
--- Conformance: a part broken into pieces (ADR 0215).
---
--- `BasePart:Fracture` cuts a box into pieces of itself, held together by
--- joints that break. What is asserted is what its page says: the pieces fill
--- the part's place, a seed is a cut, they stand together under their own
--- weight, a blow breaks them apart, and a part that is not a box is refused.
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

local function ground()
    local floor = keep(Instance.new("Part"))
    floor.Anchored = true
    floor.Size = vector.create(200, 1, 200)
    floor.Position = vector.create(0, -0.5, 0)
    floor.Parent = workspace
end

-- A wall six metres long, three high and four tenths thick, standing on the floor.
local function wall(): Part
    local made = Instance.new("Part")
    made.Name = "Wall"
    made.Anchored = true
    made.Size = vector.create(6, 3, 0.4)
    made.Position = vector.create(0, 1.5, 0)
    made.Parent = workspace
    return made
end

local function pieces(model: Model): { BasePart }
    local found: { BasePart } = {}
    for _, child in model:GetChildren() do
        if child:IsA("BasePart") and child.Name ~= "Footing" then
            table.insert(found, child :: BasePart)
        end
    end
    return found
end

describe("BasePart:Fracture", function()
    afterEach(cleanUp)

    it("puts its pieces where the part was, and the part is gone", function()
        ground()
        local original = wall()
        local model = keep(original:Fracture({ Pieces = 12, Seed = 3 }))
        expect(original.Parent):ToBe(nil)
        expect(model.Name):ToBe("Wall")
        expect(model.Parent):ToBe(workspace)
        -- It stood where it was built: it keeps its footing.
        local footing = model:FindFirstChild("Footing") :: BasePart
        expect(footing.Anchored):ToBe(true)
        expect(footing.CanCollide):ToBe(false)
        local found = pieces(model)
        expect(#found):ToBe(12)
        local volume = 0
        for _, piece in found do
            expect(piece.Anchored):ToBe(false)
            volume += piece.Size.x * piece.Size.y * piece.Size.z
            expect(math.abs(piece.Position.z) < 0.01):ToBe(true)
        end
        expect(volume):ToBeCloseTo(6 * 3 * 0.4, 0.001)
    end)

    it("cuts the same pieces from the same seed", function()
        local a = keep(wall():Fracture({ Pieces = 9, Seed = 21 }))
        local b = keep(wall():Fracture({ Pieces = 9, Seed = 21 }))
        local first, second = pieces(a), pieces(b)
        local function byName(x: BasePart, y: BasePart): boolean
            local left: string = x.Name
            local right: string = y.Name
            return left < right
        end
        table.sort(first, byName)
        table.sort(second, byName)
        for index, piece in first do
            expect(piece.Size.x):ToBe(second[index].Size.x)
            expect(piece.Size.y):ToBe(second[index].Size.y)
        end
    end)

    it("stands under its own weight, and a blow breaks it apart", function()
        ground()
        local model = keep(wall():Fracture({ Pieces = 8, Seed = 5 }))
        local broken = 0
        for _, descendant in model:GetDescendants() do
            if descendant:IsA("FixedConstraint") then
                (descendant :: FixedConstraint).Broken:Connect(function()
                    broken += 1
                end)
            end
        end
        task.wait(1)
        local highest = 0
        for _, piece in pieces(model) do
            highest = math.max(highest, piece.Position.y)
        end
        expect(highest > 2):ToBe(true)
        expect(broken):ToBe(0)

        -- A blow of many times its weight, at the top.
        local top: BasePart? = nil
        for _, piece in pieces(model) do
            if top == nil or piece.Position.y > (top :: BasePart).Position.y then
                top = piece
            end
        end
        (top :: BasePart):ApplyImpulse(vector.create(0, 0, -400))
        task.wait(1)
        expect(broken > 0):ToBe(true)
    end)

    it("refuses a part that is not a box", function()
        local ball = keep(Instance.new("Part"))
        ball.Shape = Enum.PartShape.Ball
        ball.Parent = workspace
        expect(function()
            ball:Fracture()
        end):ToThrow()
    end)
end)
