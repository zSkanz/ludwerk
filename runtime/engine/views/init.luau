--!strict
--- `@engine/views` -- mirrors and portals (ADR 0107).
---
--- Each is a `CameraTexture` whose camera this moves every tick: reflected
--- across the mirror's glass, or carried from one portal to the other. The
--- picture goes on the part's front face as an `ImageLabel` in a
--- `SurfaceGui`, and the camera's `ClipPlane` keeps whatever stands behind the
--- glass (or behind the far portal) out of it.
---
--- **The picture is exact, not an approximation.** The camera looks straight
--- through the glass, so the glass is a rectangle in its picture with its
--- edges along the picture's edges; `ImageRectOffset` and `ImageRectSize` cut
--- that rectangle out, and a negative width turns it round the way a mirror
--- does. Seen from anywhere in front of it, the mirror shows what a mirror
--- there would.
---
--- Advanced on `Heartbeat`, the tick the world's camera moves on (see
--- `@engine/camera`). A mirror in a mirror shows the other mirror empty rather
--- than showing itself: a view is absent from its own picture.
---
--- Casing follows ADR 0034: `views.mirror` is a module function; what it
--- returns is an object, so `mirror:Destroy()`.

local RunService = game:GetService("RunService")

local Views = {}

export type Surface = {
    --- The camera texture drawing the picture. Its `Resolution`,
    --- `UpdateInterval` and `Quality` are yours to change.
    Texture: CameraTexture,
    --- The camera it draws from, moved every tick.
    Camera: Camera,
    --- The picture on the part's front face.
    Picture: ImageLabel,
    --- Stops following, and removes the camera, the texture and the picture.
    Destroy: (self: Surface) -> (),
}

export type Options = {
    --- The picture's size in pixels on its longer side. 512 by default.
    Resolution: number?,
    --- The name after `view://`. Made up from the part's name by default.
    ViewName: string?,
}

local Count = 0

-- The front face of `part`: its centre, its outward normal, the direction its
-- canvas reads left to right, its up, and its width and height.
local function frontFace(part: BasePart): (vector, vector, vector, vector, number, number)
    local frame = part.CFrame
    local size = part.Size
    local normal = frame.LookVector
    return part.Position + normal * (size.z / 2), normal, -frame.RightVector, frame.UpVector, size.x, size.y
end

-- **Where the glass is in the camera's picture.** The camera at `eye` looks
-- along `normal` (straight through the glass) with `up` above it; `left`,
-- `right` and `bottom` are the canvas's top-left, top-right and bottom-left
-- corners carried into the camera's space. Returns the vertical field of view
-- that just holds them, and the canvas's rectangle in picture pixels for a
-- square picture `pixels` wide.
local function fit(
    eye: vector,
    normal: vector,
    up: vector,
    topLeft: vector,
    topRight: vector,
    bottomLeft: vector,
    bottomRight: vector,
    pixels: number
): (CFrame, number, Vector2, Vector2)
    local lens = CFrame.lookAt(eye, eye + normal, up)
    local right = lens.RightVector
    local above = lens.UpVector
    local depth = vector.dot(topLeft - eye, normal)
    local reach = 0
    for _, corner in { topLeft, topRight, bottomLeft, bottomRight } do
        local offset = corner - eye
        reach = math.max(reach, math.abs(vector.dot(offset, right)), math.abs(vector.dot(offset, above)))
    end
    local half = reach / depth
    local function pixel(corner: vector): Vector2
        local offset = corner - eye
        local x = vector.dot(offset, right) / depth / half
        local y = vector.dot(offset, above) / depth / half
        return Vector2.new((x * 0.5 + 0.5) * pixels, (0.5 - y * 0.5) * pixels)
    end
    local origin = pixel(topLeft)
    local across = pixel(topRight)
    local down = pixel(bottomLeft)
    local fieldOfView = math.deg(2 * math.atan(half))
    return lens, fieldOfView, origin, Vector2.new(across.X - origin.X, down.Y - origin.Y)
end

local function surfaceOn(part: BasePart, options: Options?, kind: string): (CameraTexture, Camera, ImageLabel, number)
    Count += 1
    local pixels = if options and options.Resolution then options.Resolution else 512
    local name = if options and options.ViewName then options.ViewName else `{kind}-{part.Name}-{Count}`

    local camera = Instance.new("Camera")
    camera.Name = `{part.Name}{kind}Camera`
    camera.ClipPlaneEnabled = true
    camera.Parent = workspace

    local texture = Instance.new("CameraTexture")
    texture.Camera = camera
    texture.ViewName = name
    texture.Resolution = Vector2.new(pixels, pixels)
    texture.Parent = camera

    local gui = Instance.new("SurfaceGui")
    gui.Face = Enum.Face.Front
    gui.Parent = part
    local picture = Instance.new("ImageLabel")
    picture.Size = UDim2.fromScale(1, 1)
    picture.BackgroundTransparency = 1
    picture.Image = `view://{name}`
    picture.Parent = gui
    return texture, camera, picture, pixels
end

local function surface(texture: CameraTexture, camera: Camera, picture: ImageLabel, step: () -> ()): Surface
    local connection = RunService.Heartbeat:Connect(function(_: number)
        step()
    end)
    step()
    local self = {
        Texture = texture,
        Camera = camera,
        Picture = picture,
    }
    function self.Destroy(_: Surface)
        connection:Disconnect()
        local gui = picture.Parent
        picture:Destroy()
        if gui then
            gui:Destroy()
        end
        texture:Destroy()
        camera:Destroy()
    end
    return self :: Surface
end

--- **A mirror on `part`'s front face.** Whatever is in front of the glass,
--- seen from `workspace.CurrentCamera` as a mirror there would show it.
---
---     local mirror = views.mirror(workspace.Bathroom.Mirror)
function Views.mirror(part: BasePart, options: Options?): Surface
    local texture, camera, picture, pixels = surfaceOn(part, options, "Mirror")
    local function step()
        local viewer = workspace.CurrentCamera
        if viewer == nil then
            return
        end
        local centre, normal, across, up, width, height = frontFace(part)
        local eye = viewer.CFrame.Position
        local distance = vector.dot(eye - centre, normal)
        -- Behind the glass, a mirror shows nothing: its back is a wall.
        texture.Enabled = distance > 0.01
        if distance <= 0.01 then
            return
        end
        -- The viewer reflected through the glass, looking back out through it.
        local reflected = eye - normal * (2 * distance)
        local halfAcross = across * (width / 2)
        local halfUp = up * (height / 2)
        local lens, fieldOfView, offset, size = fit(
            reflected,
            normal,
            up,
            centre - halfAcross + halfUp,
            centre + halfAcross + halfUp,
            centre - halfAcross - halfUp,
            centre + halfAcross - halfUp,
            pixels
        )
        camera.CFrame = lens
        camera.FieldOfView = fieldOfView
        camera.ClipPlane = CFrame.lookAt(centre, centre + normal)
        picture.ImageRectOffset = offset
        picture.ImageRectSize = size
    end
    return surface(texture, camera, picture, step)
end

--- **A portal on `a`'s front face that looks out of `b`'s.** Stepping through
--- `a` would bring you out of `b`'s front, turned round: what is in front of
--- `b` shows in `a`. The two should be the same size; a pair both ways is two
--- calls.
---
---     views.portal(workspace.BluePortal, workspace.OrangePortal)
function Views.portal(a: BasePart, b: BasePart, options: Options?): Surface
    local texture, camera, picture, pixels = surfaceOn(a, options, "Portal")
    local turn = CFrame.fromAxisAngle(vector.create(0, 1, 0), math.pi)
    local function step()
        local viewer = workspace.CurrentCamera
        if viewer == nil then
            return
        end
        local centreA, normalA, acrossA, upA, width, height = frontFace(a)
        local eye = viewer.CFrame.Position
        local distance = vector.dot(eye - centreA, normalA)
        texture.Enabled = distance > 0.01
        if distance <= 0.01 then
            return
        end
        -- From `a`'s space into `b`'s, turned round: in through the front of
        -- one, out through the front of the other.
        local carry = b.CFrame * turn * a.CFrame:Inverse()
        local centreB, normalB = frontFace(b)
        local halfAcross = acrossA * (width / 2)
        local halfUp = upA * (height / 2)
        local function through(point: vector): vector
            return carry:PointToWorldSpace(point)
        end
        local lens, fieldOfView, offset, size = fit(
            through(eye),
            normalB,
            carry:VectorToWorldSpace(upA),
            through(centreA - halfAcross + halfUp),
            through(centreA + halfAcross + halfUp),
            through(centreA - halfAcross - halfUp),
            through(centreA + halfAcross - halfUp),
            pixels
        )
        camera.CFrame = lens
        camera.FieldOfView = fieldOfView
        camera.ClipPlane = CFrame.lookAt(centreB, centreB + normalB)
        picture.ImageRectOffset = offset
        picture.ImageRectSize = size
    end
    return surface(texture, camera, picture, step)
end

return Views
