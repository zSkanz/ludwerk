--!strict
--- Mirrors and portals, from ADR 0107: `Camera.ClipPlane`, an image's source
--- rectangle, and what `@engine/views` makes and takes away. Whether the
--- picture is right is `mirrors_gate`'s question, which needs a GPU.

local testing = require("@engine/testing")
local views = require("@engine/views")

local describe, it, expect = testing.describe, testing.it, testing.expect

local function pane(position: vector, facing: vector): Part
    local part = Instance.new("Part")
    part.Anchored = true
    part.Size = vector.create(4, 3, 0.2)
    part.CFrame = CFrame.lookAt(position, position + facing)
    part.Parent = workspace
    return part
end

local function viewer(): Camera
    local camera = Instance.new("Camera")
    camera.CFrame = CFrame.lookAt(vector.create(0, 2, 0), vector.create(0, 2, -10))
    camera.Parent = workspace
    workspace.CurrentCamera = camera
    return camera
end

describe("Camera.ClipPlane", function()
    it("starts off, and takes a plane", function()
        local camera = Instance.new("Camera")
        expect(camera.ClipPlaneEnabled):ToBe(false)
        local plane = CFrame.lookAt(vector.create(0, 0, -5), vector.create(0, 0, 0))
        camera.ClipPlane = plane
        camera.ClipPlaneEnabled = true
        expect(camera.ClipPlane):ToBe(plane)
        expect(camera.ClipPlaneEnabled):ToBe(true)
        camera:Destroy()
    end)
end)

describe("ImageLabel.ImageRectSize", function()
    it("starts as the whole picture, and takes a rectangle, backwards too", function()
        local picture = Instance.new("ImageLabel")
        expect(picture.ImageRectOffset):ToBe(Vector2.zero)
        expect(picture.ImageRectSize):ToBe(Vector2.zero)
        picture.ImageRectOffset = Vector2.new(64, 0)
        picture.ImageRectSize = Vector2.new(-64, 32)
        expect(picture.ImageRectOffset):ToBe(Vector2.new(64, 0))
        expect(picture.ImageRectSize):ToBe(Vector2.new(-64, 32))
        picture:Destroy()
    end)
end)

describe("@engine/views", function()
    it("puts a mirror on a part's front face, following the world's camera", function()
        local camera = viewer()
        local glass = pane(vector.create(0, 2, -6), vector.create(0, 0, 1))
        local mirror = views.mirror(glass, { ViewName = "spec-mirror", Resolution = 256 })
        expect(mirror.Texture.ViewName):ToBe("spec-mirror")
        expect(mirror.Texture.Resolution):ToBe(Vector2.new(256, 256))
        expect(mirror.Picture.Image):ToBe("view://spec-mirror")
        expect(mirror.Camera.ClipPlaneEnabled):ToBe(true)
        -- The viewer reflected through the glass: six metres in front, six
        -- behind.
        expect(mirror.Camera.CFrame.Position.z):ToBeCloseTo(-11.8, 0.01)
        -- A mirror turns the picture round: a negative width.
        expect(mirror.Picture.ImageRectSize.X < 0):ToBe(true)
        mirror:Destroy()
        expect(glass:FindFirstChildOfClass("SurfaceGui")):ToBe(nil)
        expect(mirror.Camera.Parent):ToBe(nil)
        glass:Destroy()
        camera:Destroy()
    end)

    it("shows nothing from behind the glass", function()
        local camera = viewer()
        local glass = pane(vector.create(0, 2, -6), vector.create(0, 0, -1))
        local mirror = views.mirror(glass)
        expect(mirror.Texture.Enabled):ToBe(false)
        mirror:Destroy()
        glass:Destroy()
        camera:Destroy()
    end)

    it("carries a portal's camera to the far pane, and keeps the picture the right way round", function()
        local camera = viewer()
        local near = pane(vector.create(0, 2, -6), vector.create(0, 0, 1))
        local far = pane(vector.create(100, 2, 0), vector.create(1, 0, 0))
        local portal = views.portal(near, far)
        -- Behind the far pane, looking out of it along +X.
        expect(portal.Camera.CFrame.Position.x < 100):ToBe(true)
        expect(portal.Camera.CFrame.LookVector.x):ToBeCloseTo(1, 0.001)
        expect(portal.Picture.ImageRectSize.X > 0):ToBe(true)
        portal:Destroy()
        near:Destroy()
        far:Destroy()
        camera:Destroy()
    end)
end)
