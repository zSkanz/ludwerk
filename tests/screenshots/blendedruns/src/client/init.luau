--!strict

-- **A run of blended parts is the parts drawn one at a time.** Panes of glass
-- that overlap on the screen, in three colours and at depths that interleave,
-- so the order they are blended in is in every pixel where two cross: a run
-- that drew its members in another order than the list's would show it. Balls
-- among them, on another mesh, break the panes' runs where they stand; and a
-- solid wall behind gives the glass something to be seen against.

local Lighting = game:GetService("Lighting")

local Colours = {
    Color3.fromRGB(230, 60, 60),
    Color3.fromRGB(60, 200, 90),
    Color3.fromRGB(70, 110, 240),
}

-- One material file, and a clone a colour: the same look but for its colour,
-- which is what a run is made of.
local Glass = Material.load("asset://materials/glass.material.json" :: any)
local Looks: { [string]: Material } = {}

local function look(colour: Color3, see: number): Material
    local key = `{colour.R}/{colour.G}/{colour.B}/{see}`
    local made = Looks[key]
    if made == nil then
        made = Glass:Clone()
        made.Color = colour
        made.Transparency = see
        Looks[key] = made
    end
    return made
end

local function glass(at: vector, colour: Color3, see: number, ball: boolean?)
    local made = Instance.new("Part")
    made.Anchored = true
    made.Size = if ball then vector.create(2.4, 2.4, 2.4) else vector.create(3.2, 3.2, 0.2)
    if ball then
        made.Shape = Enum.PartShape.Ball
    end
    made.Position = at
    made.Material = look(colour, see)
    made.CastShadow = false
    made.Parent = workspace
end

local wall = Instance.new("Part")
wall.Anchored = true
wall.Size = vector.create(40, 16, 1)
wall.Position = vector.create(0, 4, -8)
wall.Parent = workspace

-- Eighteen panes in a fan: each overlaps its neighbours, colours in turn, and
-- depth going back and forth so no colour's panes are all in front.
-- **Each at a transparency of its own**, as the parts of an effect are, every
-- one at another age: see-through is the instance's, as colour is, and does
-- not split a run.
for index = 0, 17 do
    local colour = Colours[index % 3 + 1]
    local depth = (index * 7 % 5) * 0.9
    glass(vector.create(index * 1.4 - 12, 3 + (index % 2) * 1.2, depth), colour, 0.3 + index * 0.03)
end
-- Six balls of one colour among them, a run of their own on another mesh.
for index = 0, 5 do
    glass(vector.create(index * 4 - 10, 1.4, 2.2 + (index % 3) * 0.8), Colours[2], 0.5, true)
end

local camera = Instance.new("Camera")
camera.FieldOfView = 50
camera.CFrame = CFrame.lookAt(vector.create(0, 4, 22), vector.create(0, 3, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

Lighting.ClockTime = 14
Lighting.FogStart = 100000
Lighting.FogEnd = 200000
