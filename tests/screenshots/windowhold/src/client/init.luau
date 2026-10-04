--!strict
--[[
	A window held by the system (D535): a part, a camera, and nothing else.

	`run_window_hold_gate.cmake` holds the window for two seconds, as Windows
	does while a title bar is held, and asks that no frame of the run took
	anything like that long: the loop went on simulating and drawing through it.
]]

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(0, 4, 10), vector.create(0, 1, 0))

local part = Instance.new("Part")
part.Size = vector.create(2, 2, 2)
part.CFrame = CFrame.new(0, 1, 0)
part.Anchored = true
part.Parent = workspace
