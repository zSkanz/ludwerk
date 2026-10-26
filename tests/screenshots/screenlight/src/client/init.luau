--!strict
--[[
	The scenes for `screen_light_gate` (ADR 0209): what a surface reflects of
	the screen, and the light that bounces off it. One project, and the scene
	is the one `--scene-data={"scene":"..."}` names:

	- `mirror`: a polished floor in a room with a `ReflectionProbe`, and a red
	  box that is MOVED after the probe took its picture. With the screen's
	  reflections on, the floor shows the box where it stands now and the wall
	  where it stood; with them off it is the probe's picture, which has it
	  where it was.
	- `ground`: the same wall on a terrain, whose ground reads the screen as a
  part does (ADR 0263).
- `wall`: a white floor and a red wall standing on it, glowing. With the
	  screen's bounce on, the floor at the wall's foot is reddened. With
	  `"late": true` the script turns the quality level to `Ultra` a moment in
	  -- the level that turns both on -- so the picture is of a setting that
	  arrived while the game ran.
	- `ball`: a chrome ball on that polished floor. A ball is shaded smooth
	  and made of facets, and the screen's ray is walked from the facets: the
	  ball keeps the probe's reflection, exactly.
	- `lake`: a flat lake, three posts on its far bank, the camera low on the
	  near one. With `"mirror": true` there is no water and the camera is the
	  one mirrored in the water's plane: flipped top to bottom, its picture is
	  what a flat lake's mirror shows. With `"empty": true` as well there is
	  nothing at all, and its picture is the sky that camera sees -- what the
	  water reflects where there is nothing to (`imgmirror`).

	Nothing moves but the one box, once; the exposure is the world's and fixed.
]]

local GraphicsService = game:GetService("GraphicsService")
local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

type Asked = { scene: string?, late: boolean?, mirror: boolean?, empty: boolean? }
local asked = (SceneService:GetLoadData() or {}) :: Asked
local scene = asked.scene or "mirror"

Lighting.AutoExposure = false

local function part(name: string, size: vector, position: vector, color: Color3): Part
    local made = Instance.new("Part")
    made.Name = name
    made.Anchored = true
    made.Size = size
    made.Position = position
    made:SetMaterialParameter("Color", color)
    made.Parent = workspace
    return made
end

local camera = Instance.new("Camera")
camera.Parent = workspace
workspace.CurrentCamera = camera

if scene == "mirror" or scene == "ball" then
    Lighting.ClockTime = 14
    local floor = part("Floor", vector.create(30, 1, 30), vector.create(0, -0.5, 0), Color3.fromRGB(40, 40, 46))
    floor.Material = Material.load("asset://materials/polished.material.json")
    part("Wall", vector.create(30, 8, 1), vector.create(0, 4, -14), Color3.fromRGB(200, 190, 160))

    -- The room a probe pictures: an invisible part its size, and the probe
    -- under it.
    local room = part("Room", vector.create(30, 10, 30), vector.create(0, 5, 0), Color3.new(1, 1, 1))
    room:SetMaterialParameter("Transparency", 1)
    room.CanCollide = false
    room.CanQuery = false
    local probe = Instance.new("ReflectionProbe")
    probe.Parent = room

    if scene == "mirror" then
        -- Where the probe pictures it, and where it stands when the picture
        -- of the screen is taken.
        local red = part("Red", vector.create(2, 4, 2), vector.create(-4, 2, -6), Color3.fromRGB(220, 30, 30))
        local tick = 0
        RunService.Heartbeat:Connect(function()
            tick += 1
            if tick == 12 then
                red.Position = vector.create(4, 2, -6)
            end
        end)
    else
        local ball = part("Ball", vector.create(3, 3, 3), vector.create(0, 1.5, -3), Color3.new(1, 1, 1))
        ball.Shape = Enum.PartShape.Ball
        ball.Material = Material.load("asset://materials/chrome.material.json")
        part("Red", vector.create(2, 4, 2), vector.create(-5, 2, -6), Color3.fromRGB(220, 30, 30))
        part("Blue", vector.create(2, 2, 2), vector.create(5, 1, -5), Color3.fromRGB(40, 80, 230))
    end
    camera.CFrame = CFrame.lookAt(vector.create(0, 3.2, 9), vector.create(0, 1.2, -5))
elseif scene == "wall" then
    Lighting.ClockTime = 12
    part("Floor", vector.create(40, 1, 40), vector.create(0, -0.5, 0), Color3.fromRGB(235, 235, 235))
    -- It glows, a little: a wall lit by the sun reddens a sunlit floor by a
    -- few levels of 255, which is true and no claim a picture can hold. What
    -- a glowing wall gives the floor comes by no other road than the bounce
    -- -- a glow is no lamp -- so the floor at its foot is red with it and
    -- grey without.
    local red = part("Red", vector.create(12, 6, 1), vector.create(0, 3, -3), Color3.fromRGB(230, 20, 20))
    red:SetMaterialParameter("Emissive", Color3.new(4, 0.15, 0.15))
    camera.CFrame = CFrame.lookAt(vector.create(2, 5, 9), vector.create(-1, 0.5, -1))
    if asked.late then
        local tick = 0
        RunService.Heartbeat:Connect(function()
            tick += 1
            if tick == 10 then
                GraphicsService.QualityLevel = Enum.GraphicsQuality.Ultra
            end
        end)
    end
elseif scene == "ground" then
    -- **The ground reads the screen too** (ADR 0263): the same glowing wall,
    -- standing on a terrain instead of a part. With the bounce on the ground
    -- at its foot is reddened, as the floor is.
    Lighting.ClockTime = 12
    local terrain = Instance.new("Terrain")
    terrain.Name = "Ground"
    terrain.MinHeight = -64
    terrain.MaxHeight = 64
    terrain:SetLayers({ "engine://terrain/sand" })
    terrain:FillBlock(vector.create(0, -2, 0), vector.create(40, 4, 40), 1)
    terrain.Parent = workspace
    local red = part("Red", vector.create(12, 6, 1), vector.create(0, 3, -3), Color3.fromRGB(230, 20, 20))
    red:SetMaterialParameter("Emissive", Color3.new(4, 0.15, 0.15))
    camera.CFrame = CFrame.lookAt(vector.create(2, 5, 9), vector.create(-1, 0.5, -1))
elseif scene == "lake" then
    Lighting.ClockTime = 13
    local WaterLevel = 0
    if not asked.empty then
        part("Bed", vector.create(120, 1, 120), vector.create(0, -3.5, 0), Color3.fromRGB(190, 180, 140))
        part("FarBank", vector.create(120, 5, 20), vector.create(0, -1.5, -45), Color3.fromRGB(190, 180, 140))
        part("NearBank", vector.create(120, 5, 20), vector.create(0, -1.5, 40), Color3.fromRGB(190, 180, 140))
        local colours = { Color3.fromRGB(230, 30, 30), Color3.fromRGB(250, 220, 40), Color3.fromRGB(240, 240, 240) }
        for index = 1, 3 do
            part(`Post{index}`, vector.create(2, 12, 2), vector.create((index - 2) * 8, 7, -38), colours[index])
        end
        -- A beam hung over the water, nearer the camera than the posts and
        -- across them in the picture. A ray from the far water passes BEHIND
        -- it on its way to a post: the beam is in the picture there and the
        -- ray is forty metres beyond it. That is the landing a ray must not
        -- take -- the beam's colour laid across water whose mirror shows a
        -- post or the sky -- and the beam's own reflection is elsewhere, in
        -- the near water.
        part("Beam", vector.create(40, 1, 1), vector.create(0, 4.5, 10), Color3.fromRGB(230, 40, 220))
    end
    local from = vector.create(0, WaterLevel + 2.6, 31)
    local at = vector.create(0, WaterLevel + 3, -38)
    if asked.mirror then
        local function mirrored(point: vector): vector
            return vector.create(point.x, 2 * WaterLevel - point.y, point.z)
        end
        camera.CFrame = CFrame.lookAt(mirrored(from), mirrored(at))
    else
        local lake = Instance.new("Water")
        lake.Name = "Lake"
        lake.Shape = Enum.WaterShape.Box
        lake.Position = vector.create(0, -1.5, 0)
        lake.Size = vector.create(120, 3, 70)
        lake.SurfaceLevel = WaterLevel
        lake.Parent = workspace
        camera.CFrame = CFrame.lookAt(from, at)
    end
else
    error(`screenlight: no scene named "{scene}"`)
end
