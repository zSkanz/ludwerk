--!strict
--[[
	A decal's three ways of meeting a surface (ADR 0160), each a red square on
	a grey floor seen from straight above, and `run_decal_blend_gate.cmake`
	asks of each whether it left the floor darker or brighter:

	  left    Multiply -- what a decal always was: it tints, and darkens.
	  middle  Alpha, glowing -- laid over the floor, brighter than it.
	  right   Additive -- its light added, brighter than the floor.

	An orthographic camera twenty metres wide over a 1000 by 500 window, so a
	square's middle is at a known place: x = (metres + 10) / 20.
]]

local Lighting = game:GetService("Lighting")

Lighting.ClockTime = 12

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.Projection = Enum.CameraProjection.Orthographic
camera.OrthographicSize = 5
camera.CFrame = CFrame.lookAt(vector.create(0, 30, 0), vector.create(0, 0, 0), vector.create(0, 0, -1))

local floor = Instance.new("Part")
floor.Size = vector.create(60, 1, 60)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor:SetMaterialParameter("Color", Color3.fromRGB(110, 110, 110))
floor.Anchored = true
floor.Parent = workspace

local function mark(x: number, mode: EnumDecalBlendMode, emissive: number)
    local decal = Instance.new("Decal")
    decal.CFrame = CFrame.lookAt(vector.create(x, 0, 0), vector.create(x, 1, 0))
    decal.Size = vector.create(3, 3, 1)
    decal.Color = Color3.fromRGB(255, 40, 30)
    decal.BlendMode = mode
    decal.Emissive = emissive
    decal.Parent = workspace
end

mark(-6, Enum.DecalBlendMode.Multiply, 0)
mark(0, Enum.DecalBlendMode.Alpha, 1.5)
mark(6, Enum.DecalBlendMode.Additive, 0)
