--!strict

-- **A crowd of one skinned mesh** (H2): nine copies of a two-joint bar in a
-- square, each bending through its clip at its own speed, under the sun.
-- Drawn as one run -- each copy's palette read from the frame's palette buffer
-- at the place its instance names -- and drawn a copy at a time
-- (`--no-instancing`), the two pictures must be the same: a run that posed
-- every copy with the first one's palette, or with nobody's, is a picture of
-- nine identical bars, or nine bars standing straight.

local Lighting = game:GetService("Lighting")

local camera = workspace.CurrentCamera or Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(0, 3, 7), vector.create(0, 0.5, 0))
Lighting.GlobalShadows = true

local floor = Instance.new("Part")
floor.Anchored = true
floor.Size = vector.create(20, 1, 20)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor.Parent = workspace

for index = 0, 8 do
    local model = Instance.new("Model")
    local bar = Instance.new("MeshPart")
    bar.MeshContent = "asset://models/bar.gltf" :: Content
    bar.Anchored = true
    bar.CFrame = CFrame.new((index % 3 - 1) * 1.6, 0, (index // 3 - 1) * 1.6)
    bar.Parent = model
    local player = Instance.new("AnimationPlayer")
    player.Parent = model
    local bend = player:LoadAnimation("Bend" :: Content)
    bend.Looped = true
    -- Each at its own speed, so no two are posed alike.
    bend.Speed = 0.4 + index * 0.15
    bend:Play()
    model.Parent = workspace
end
