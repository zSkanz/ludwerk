--!strict

-- **A morphed vertex lands where the file says** (ADR 0196). Four pillars on
-- a floor under a low sun: three of one mesh with no skeleton, and one skinned
-- whose joint a clip holds moved and turned.
--
-- The driver runs this three times. With `content/models` as checked in:
-- the pillars are a unit tall at rest and their file's own weights make them
-- two tall, leaning, through the morph pipelines. With the files of `driven`
-- in their place: the same targets with every weight at nought in the file,
-- so that what shapes the pillars is this script's `SetMorphWeight` on the
-- plain ones and the weight channels of the skinned one's clip. And with the
-- files of `made`: the same pillars with no targets, already that shape. The
-- three pictures must be the same -- the lit faces, the depth the prepass
-- wrote and the shadows on the floor -- and a target left at nought, which
-- would throw every vertex three units out, must not show in any.
--
-- **Three of the plain one**, because three of a mesh are a run: made without
-- targets they are drawn in one call, and with weights above nought each is
-- drawn alone. The same picture either way is also the statement that a body
-- leaving its run to be morphed is drawn as it was in it.

local Lighting = game:GetService("Lighting")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(30, 1, 30)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(200, 200, 200))
floor.Parent = workspace

for index, x in { -3.2, -1.6, 0 } do
    local pillar = Instance.new("MeshPart")
    pillar.Name = `Plain{index}`
    pillar.MeshContent = "asset://models/plain.gltf" :: Content
    pillar.Anchored = true
    -- Each turned a little more than the last, so no two faces are lit alike.
    pillar.CFrame = CFrame.new(x, 0, 0) * CFrame.fromEuler(0, index * 0.5, 0)
    pillar.Parent = workspace
    -- **Set before the file has arrived**, by name, and kept until it does:
    -- the weights the checked-in file gives itself, so that run is unmoved by
    -- them; the whole of the shape where the file's are nought; and nothing
    -- at all on a mesh that turns out to have no such targets.
    pillar:SetMorphWeight("Tall", 0.5)
    pillar:SetMorphWeight("Lean", -0.25)
end

local model = Instance.new("Model")
local skinned = Instance.new("MeshPart")
skinned.Name = "Skinned"
skinned.MeshContent = "asset://models/skinned.gltf" :: Content
skinned.Anchored = true
skinned.CFrame = CFrame.new(1.8, 0, 0) * CFrame.fromEuler(0, 0.4, 0)
skinned.Parent = model
local player = Instance.new("AnimationPlayer")
player.Parent = model
local hold = player:LoadAnimation("Hold" :: Content)
hold.Looped = true
hold:Play()
model.Parent = workspace

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(-0.5, 3.2, 8.5), vector.create(-0.5, 1.0, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

-- The sun in the east, half way up: every pillar's shadow lies along the
-- floor to its left, as long as the pillar is tall. The exposure held still,
-- so the two runs are the same picture and not two adaptations of it.
Lighting.GeographicLatitude = 0
Lighting.ClockTime = 9
Lighting.GlobalShadows = true
Lighting.AutoExposure = false
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
