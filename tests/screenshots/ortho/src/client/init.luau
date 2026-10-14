--!strict
-- `ortho_gate`: a top-down orthographic camera standing LOWER than what it
-- looks at -- a metre up, over a ball two metres up and blocks, a cylinder and
-- a wedge taller than that. An orthographic camera sees its whole column
-- (`render_world.cpp`), so every one of them is in the picture, whatever its
-- shape. Before, what stood wholly above the camera was culled -- the ball
-- vanished and left its shadow -- and, once depth was clipped, so did the
-- tops of the rest.

local function part(name: string, shape: EnumPartShape, size: vector, position: vector, color: Color3)
    local p = Instance.new("Part")
    p.Name = name
    p.Anchored = true
    p.Shape = shape
    p.Size = size
    p.CFrame = CFrame.new(position.x, position.y, position.z)
    p:SetMaterialParameter("Color", color)
    p.Parent = workspace
end

part("Floor", Enum.PartShape.Block, vector.create(22, 1, 22), vector.create(0, -0.5, 0), Color3.fromRGB(60, 200, 60))
for index = 0, 2 do
    part(
        "Block",
        Enum.PartShape.Block,
        vector.create(2, 2, 2),
        vector.create(4 + index * 2, 1, -8),
        Color3.fromRGB(60, 170, 250)
    )
end
part("Ball", Enum.PartShape.Ball, vector.create(1.5, 1.5, 1.5), vector.create(3, 2, -7), Color3.fromRGB(240, 120, 140))
part(
    "Cylinder",
    Enum.PartShape.Cylinder,
    vector.create(2, 3, 2),
    vector.create(0, 1.5, 4),
    Color3.fromRGB(250, 200, 40)
)
part("Wedge", Enum.PartShape.Wedge, vector.create(2, 3, 2), vector.create(-3, 1.5, 4), Color3.fromRGB(250, 200, 40))

local camera = Instance.new("Camera")
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 12
camera.CFrame = CFrame.lookAt(vector.create(0, 1, 0), vector.create(0, 0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
