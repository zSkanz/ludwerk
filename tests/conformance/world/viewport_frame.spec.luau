--!strict
--- `ViewportFrame`, from ADR 0107: its defaults, what may live in it, and the
--- one promise about what is inside -- it is shown, not simulated. Whether it
--- draws is `views_gate`'s question, which needs a GPU.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local UIService = game:GetService("UIService")

local function frameOnScreen(): (ScreenGui, ViewportFrame)
    local screen = Instance.new("ScreenGui")
    screen.Parent = UIService
    local frame = Instance.new("ViewportFrame")
    frame.Size = UDim2.fromOffset(200, 200)
    frame.Parent = screen
    return screen, frame
end

describe("ViewportFrame", function()
    it("starts with no camera and a light from above and behind", function()
        local frame = Instance.new("ViewportFrame")
        expect(frame.CurrentCamera):ToBe(nil)
        expect(frame.Ambient):ToBe(Color3.fromRGB(140, 140, 150))
        expect(frame.LightColor):ToBe(Color3.new(1, 1, 1))
        expect(frame.LightDirection):ToBe(vector.create(-1, -1, -1))
        frame:Destroy()
    end)

    it("holds parts, models and its own camera", function()
        local screen, frame = frameOnScreen()
        local item = Instance.new("Part")
        item.Parent = frame
        local model = Instance.new("Model")
        model.Parent = frame
        local lens = Instance.new("Camera")
        lens.Parent = frame
        frame.CurrentCamera = lens
        expect(item.Parent):ToBe(frame)
        expect(model.Parent):ToBe(frame)
        expect(frame.CurrentCamera):ToBe(lens)
        screen:Destroy()
    end)

    it("refuses a camera that is not one, and a light with no direction", function()
        local frame = Instance.new("ViewportFrame")
        expect(function()
            (frame :: any).CurrentCamera = Instance.new("Part")
        end):ToThrow()
        expect(function()
            frame.LightDirection = vector.create(0, 0, 0)
        end):ToThrow()
        frame:Destroy()
    end)

    it("does not simulate what is inside: a loose part stays where it is put", function()
        local screen, frame = frameOnScreen()
        local item = Instance.new("Part")
        item.Anchored = false
        item.Position = vector.create(0, 10, 0)
        item.Parent = frame
        task.wait(1)
        expect(item.Position.y):ToBeCloseTo(10, 0.001)
        screen:Destroy()
    end)
end)
