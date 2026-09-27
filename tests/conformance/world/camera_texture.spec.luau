--!strict
--- `CameraTexture`, from ADR 0107: its defaults, what it refuses, and where it
--- may be parented. Whether it draws is `views_gate`'s question, which needs a
--- GPU; this is the API a script holds.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("CameraTexture", function()
    it("starts as a simple feed, every frame, from no camera", function()
        local feed = Instance.new("CameraTexture")
        expect(feed.Camera):ToBe(nil)
        expect(feed.ViewName):ToBe("")
        expect(feed.Resolution):ToBe(Vector2.new(512, 288))
        expect(feed.Enabled):ToBe(true)
        expect(feed.UpdateInterval):ToBe(1)
        expect(feed.Quality):ToBe(Enum.ViewQuality.Simple)
        feed:Destroy()
    end)

    it("takes a camera, a name, a size, an interval and a quality", function()
        local lens = Instance.new("Camera")
        lens.Parent = workspace
        local feed = Instance.new("CameraTexture")
        feed.Camera = lens
        feed.ViewName = "lobby"
        feed.Resolution = Vector2.new(320, 180)
        feed.UpdateInterval = 4
        feed.Quality = Enum.ViewQuality.Full
        feed.Enabled = false
        expect(feed.Camera):ToBe(lens)
        expect(feed.ViewName):ToBe("lobby")
        expect(feed.Resolution):ToBe(Vector2.new(320, 180))
        expect(feed.UpdateInterval):ToBe(4)
        expect(feed.Quality):ToBe(Enum.ViewQuality.Full)
        expect(feed.Enabled):ToBe(false)
        feed.Camera = nil
        expect(feed.Camera):ToBe(nil)
        feed:Destroy()
        lens:Destroy()
    end)

    it("refuses what is not a camera, a size below a pixel, and a part of a frame", function()
        local feed = Instance.new("CameraTexture")
        expect(function()
            (feed :: any).Camera = Instance.new("Part")
        end):ToThrow()
        expect(function()
            feed.Resolution = Vector2.new(0, 100)
        end):ToThrow()
        expect(function()
            feed.UpdateInterval = 0
        end):ToThrow()
        expect(function()
            feed.UpdateInterval = 1.5
        end):ToThrow()
        feed:Destroy()
    end)

    it("lives in the world, in a model, on a part, or beside its camera", function()
        local lens = Instance.new("Camera")
        lens.Parent = workspace
        local feed = Instance.new("CameraTexture")
        feed.Parent = workspace
        expect(feed.Parent):ToBe(workspace)
        feed.Parent = lens
        expect(feed.Parent):ToBe(lens)
        feed:Destroy()
        lens:Destroy()
    end)
end)
