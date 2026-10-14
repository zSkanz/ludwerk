--!strict
--- Indirect light (ADRs 0202, 0203): what a room reflects, and the light its
--- own walls give back.
---
--- Two rooms that share a wall ten centimetres thick, with a door in it. The
--- west one has a window the sun comes through; the east one has no window at
--- all. Each has an invisible part its own size, and under that part:
---
---   * a `ReflectionProbe`, so the polished floor and the chrome ball reflect
---     the room they are in and not the sky they cannot see; and
---   * an `IrradianceVolume`, so the room is lit by what its walls bounce --
---     the red wall reddens the floor beside it, the sunlit patch lights the
---     ceiling -- and the dark room by what comes through its door, and by
---     nothing that comes through its wall.
---
--- A walker goes from one room to the other and back, and is lit by whichever
--- it is in: nothing here is baked into the walls, so what moves is lit like
--- what does not.
---
--- And an annex beside the house that is ONE mesh, as a building from a
--- modelling program is: lit through the window and the door it has, and
--- tinted by its tiled floor.
---
---   1, 2, 3  look at the bright room, the dark room, the annex
---   G      bounced light off and on    (`GraphicsService.GlobalIllumination`)
---   R      room reflections off and on (`GraphicsService.Reflections`)
---   S      reflections from the screen off and on: the walker in the
---          polished floor, which no probe has
---          (`GraphicsService.ScreenSpaceReflections`, ADR 0209)
---   B      light bounced off the screen off and on: the red wall on the
---          floor at its foot, finer than a volume's grain
---          (`GraphicsService.ScreenSpaceBounce`)
---   L      a lamp in the dark room off and on; both rooms follow, since
---          their probes and volumes are `Dynamic`

local GraphicsService = game:GetService("GraphicsService")
local InputService = game:GetService("InputService")
local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 13
Lighting.GeographicLatitude = 42

-- The house: sixteen metres by eight, the dividing wall across the middle.
local Width = 16
local Depth = 8
local Height = 3.2
local Shell = 0.3
local Thin = 0.1
local DoorHalf = 0.7
local DoorHeight = 2.2

local house = Instance.new("Folder")
house.Name = "House"
house.Parent = workspace

local function slab(name: string, size: vector, position: vector, color: Color3): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Anchored = true
    part.Parent = house
    return part
end

local Plaster = Color3.fromRGB(226, 222, 212)
local Red = Color3.fromRGB(200, 40, 34)

local floor = slab("Floor", vector.create(Width, 0.5, Depth), vector.create(0, -0.25, 0), Plaster)
floor.Material = Material.load("asset://materials/polished.material.json")
slab(
    "Ceiling",
    vector.create(Width + 2 * Shell, Shell, Depth + 2 * Shell),
    vector.create(0, Height + Shell / 2, 0),
    Plaster
)
slab(
    "North",
    vector.create(Width + 2 * Shell, Height, Shell),
    vector.create(0, Height / 2, -Depth / 2 - Shell / 2),
    Plaster
)
slab("East", vector.create(Shell, Height, Depth), vector.create(Width / 2 + Shell / 2, Height / 2, 0), Plaster)
-- The wall the sun never reaches directly, in a colour a floor can catch.
slab("West", vector.create(Shell, Height, Depth), vector.create(-Width / 2 - Shell / 2, Height / 2, 0), Red)

-- The south wall, with a window into the west room: four pieces round a hole
-- four metres wide.
local southZ = Depth / 2 + Shell / 2
local WindowLeft, WindowRight, WindowSill, WindowTop = -6, -2, 0.8, 2.6
slab(
    "SouthLeft",
    vector.create(WindowLeft + Width / 2 + Shell, Height, Shell),
    vector.create((WindowLeft - Width / 2 - Shell) / 2, Height / 2, southZ),
    Plaster
)
slab(
    "SouthRight",
    vector.create(Width / 2 + Shell - WindowRight, Height, Shell),
    vector.create((Width / 2 + Shell + WindowRight) / 2, Height / 2, southZ),
    Plaster
)
slab(
    "SouthSill",
    vector.create(WindowRight - WindowLeft, WindowSill, Shell),
    vector.create((WindowLeft + WindowRight) / 2, WindowSill / 2, southZ),
    Plaster
)
slab(
    "SouthHead",
    vector.create(WindowRight - WindowLeft, Height - WindowTop, Shell),
    vector.create((WindowLeft + WindowRight) / 2, (Height + WindowTop) / 2, southZ),
    Plaster
)

-- **The thin wall**: ten centimetres, with a door.
local wallLength = Depth / 2 - DoorHalf
slab(
    "DividerNorth",
    vector.create(Thin, Height, wallLength),
    vector.create(0, Height / 2, -DoorHalf - wallLength / 2),
    Plaster
)
slab(
    "DividerSouth",
    vector.create(Thin, Height, wallLength),
    vector.create(0, Height / 2, DoorHalf + wallLength / 2),
    Plaster
)
slab(
    "DividerLintel",
    vector.create(Thin, Height - DoorHeight, 2 * DoorHalf),
    vector.create(0, (Height + DoorHeight) / 2, 0),
    Plaster
)

-- Something in each room to see the room in.
local chrome = Material.load("asset://materials/chrome.material.json")
local function ball(name: string, position: vector)
    local part = Instance.new("Part")
    part.Name = name
    part.Shape = Enum.PartShape.Ball
    part.Size = vector.create(1.2, 1.2, 1.2)
    part.Position = position
    part.Material = chrome
    part.Anchored = true
    part.Parent = house
end
ball("BrightBall", vector.create(-5.5, 0.6, -2))
ball("DarkBall", vector.create(5.5, 0.6, -2))
slab("Crate", vector.create(1.2, 1.2, 1.2), vector.create(4, 0.6, 2.2), Color3.fromRGB(240, 240, 240))

-- **A room is an invisible part its own size**, and what stands under it. The
-- two parts meet in the middle of the thin wall: a surface reads the volume
-- that a short step in front of it is inside, so each face of the wall is lit
-- by its own room however thin the wall is.
local lamp: PointLight? = nil
local function room(name: string, centre: vector, size: vector)
    local box = Instance.new("Part")
    box.Name = name
    box.Size = size
    box.Position = centre
    box:SetMaterialParameter("Transparency", 1)
    box.CanCollide = false
    box.CanQuery = false
    box.Anchored = true
    box.Parent = house

    local probe = Instance.new("ReflectionProbe")
    probe.Blend = 0.4
    -- Taken again when the lamp is switched: a lamp is part of what a room is.
    probe.Dynamic = true
    probe.Parent = box

    local volume = Instance.new("IrradianceVolume")
    volume.Spacing = 1
    volume.Bounces = 2
    volume.Dynamic = true
    volume.Parent = box
end
room("BrightRoom", vector.create(-Width / 4, Height / 2, 0), vector.create(Width / 2, Height, Depth))
room("DarkRoom", vector.create(Width / 4, Height / 2, 0), vector.create(Width / 2, Height, Depth))

-- **The annex: a room that is ONE mesh.** Its walls, its ceiling, its tiled
-- floor, the window and the door in its south wall are a single file
-- (`tools/make_annex.py` writes it), which is how a building arrives from a
-- modelling program. To the physics world a mesh is one hull, its door and
-- its window shut; the light is worked out against its own triangles instead,
-- so the sun comes in through the openings it has -- and what it lands on is
-- terracotta, by the mean of the floor's picture, and so is what the walls
-- get back.
local AnnexZ = -14
do
    local annex = Instance.new("MeshPart")
    annex.Name = "Annex"
    annex.MeshContent = "asset://models/annex.gltf" :: Content
    annex.CFrame = CFrame.new(0, 0, AnnexZ)
    annex.Anchored = true
    annex.Parent = house
    slab("AnnexGround", vector.create(14, 0.5, 12), vector.create(0, -0.5, AnnexZ + 1), Color3.fromRGB(120, 124, 110))
end
room("AnnexRoom", vector.create(0, 1.5, AnnexZ), vector.create(6, 3, 6))

do
    local holder = Instance.new("Part")
    holder.Name = "Lamp"
    holder.Size = vector.create(0.3, 0.3, 0.3)
    holder.Position = vector.create(4, Height - 0.4, 0)
    holder:SetMaterialParameter("Color", Color3.fromRGB(255, 244, 214))
    holder.Anchored = true
    holder.CanCollide = false
    holder.CanQuery = false
    holder.CastShadow = false
    holder.Parent = house
    local light = Instance.new("PointLight")
    light.Color = Color3.fromRGB(255, 226, 170)
    light.Brightness = 3
    light.Range = 12
    light.Enabled = false
    light.Parent = holder
    lamp = light
end

-- --- The walker --------------------------------------------------------------

local walker = Instance.new("CharacterBody")
walker.Name = "Walker"
walker.Size = vector.create(0.8, 1.8, 0.8)
walker.Position = vector.create(-5, 1, 0)
walker:SetMaterialParameter("Color", Color3.fromRGB(236, 236, 236))
walker.WalkSpeed = 2.5
walker.Parent = workspace

local heading = 1
RunService.PreSimulation:Connect(function()
    local x = walker.Position.x
    if x > 5 then
        heading = -1
    elseif x < -5 then
        heading = 1
    end
    -- Straight through the door, which is at z = 0.
    walker:Move(vector.normalize(vector.create(heading, 0, -walker.Position.z)))
end)

-- --- The view ----------------------------------------------------------------

local camera = Instance.new("Camera")
camera.Name = "Indoors"
camera.FieldOfView = 70
camera.Parent = workspace
workspace.CurrentCamera = camera

local function look(room: number)
    if room == 1 then
        camera.CFrame = CFrame.lookAt(vector.create(-7.4, 1.7, -3.4), vector.create(0.5, 1.0, 1.0))
    elseif room == 2 then
        camera.CFrame = CFrame.lookAt(vector.create(7.4, 1.7, -3.4), vector.create(-0.5, 1.0, 1.0))
    else
        -- Inside the annex, from its north-east corner towards its openings.
        camera.CFrame = CFrame.lookAt(vector.create(2.6, 1.7, AnnexZ - 2.6), vector.create(-1.0, 0.9, AnnexZ + 2.5))
    end
end
look(1)

InputService.InputBegan:Connect(function(input: InputObject, consumed: boolean)
    if consumed then
        return
    end
    if input.KeyCode == Enum.KeyCode.Digit1 then
        look(1)
    elseif input.KeyCode == Enum.KeyCode.Digit2 then
        look(2)
    elseif input.KeyCode == Enum.KeyCode.Digit3 then
        look(3)
    elseif input.KeyCode == Enum.KeyCode.G then
        local on = GraphicsService.GlobalIllumination ~= Enum.GraphicsLevel.Low
        GraphicsService.GlobalIllumination = if on then Enum.GraphicsLevel.Low else Enum.GraphicsLevel.High
        print(`[interior] bounced light {if on then "off" else "on"}`)
    elseif input.KeyCode == Enum.KeyCode.R then
        local on = GraphicsService.Reflections ~= Enum.GraphicsLevel.Low
        GraphicsService.Reflections = if on then Enum.GraphicsLevel.Low else Enum.GraphicsLevel.High
        print(`[interior] room reflections {if on then "off" else "on"}`)
    elseif input.KeyCode == Enum.KeyCode.S then
        -- Off at every level but `Ultra`: a game that wants it says so.
        local on = GraphicsService.ScreenSpaceReflections ~= Enum.GraphicsLevel.Low
        GraphicsService.ScreenSpaceReflections = if on then Enum.GraphicsLevel.Low else Enum.GraphicsLevel.Ultra
        print(`[interior] reflections from the screen {if on then "off" else "on"}`)
    elseif input.KeyCode == Enum.KeyCode.B then
        local on = GraphicsService.ScreenSpaceBounce ~= Enum.GraphicsLevel.Low
        GraphicsService.ScreenSpaceBounce = if on then Enum.GraphicsLevel.Low else Enum.GraphicsLevel.Ultra
        print(`[interior] light bounced off the screen {if on then "off" else "on"}`)
    elseif input.KeyCode == Enum.KeyCode.L and lamp ~= nil then
        lamp.Enabled = not lamp.Enabled
        print(`[interior] lamp {if lamp.Enabled then "on" else "off"}`)
    end
end)
