--!strict

-- Sixty-four sounds behind walls (ADR 0255): what looking for the walls
-- between a listener and its sounds costs a frame, on whatever machine runs
-- it -- the frame report says it in its occlusion line.
--
--   * **Eight rows of eight sounds**, each row behind a wall, and the walls
--     one behind the other, so the far rows are behind several.
--   * **A listener turning on the spot**, so every sound's rays change from
--     frame to frame and none is a ray already answered.
--
-- Run by hand rather than by CTest, for the reason `tests/perf/README.md`
-- gives. `OcclusionRays` is the quality level's unless the command line's
-- script sets it.

local RunService = game:GetService("RunService")

game:GetService("AudioService").OcclusionEnabled = true

for row = 0, 7 do
    local wall = Instance.new("Part")
    wall.Anchored = true
    wall.Size = vector.create(40, 8, 0.5)
    wall.Position = vector.create(0, 2, -6 - row * 4)
    wall.Parent = workspace
    for column = 0, 7 do
        local holder = Instance.new("Part")
        holder.Anchored = true
        holder.Size = vector.create(1, 1, 1)
        holder.Position = vector.create(column * 4 - 14, 2, -8 - row * 4)
        holder.Parent = workspace
        local heard = Instance.new("Sound")
        heard.Looped = true
        heard.Volume = 0.05
        heard.RollOffMaxDistance = 100
        heard.Parent = holder
        heard:Play()
    end
end

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera
local turned = 0
RunService.Heartbeat:Connect(function(dt: number)
    turned += dt * 0.5
    camera.CFrame = CFrame.lookAt(vector.create(0, 2, 0), vector.create(math.sin(turned) * 10, 2, -10))
end)
