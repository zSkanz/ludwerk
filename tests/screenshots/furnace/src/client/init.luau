--!strict
--- The white furnace (ADR 0203): a room shut on every side, every wall one
--- grey, one lamp at its middle and nothing else. What its walls give back is
--- known -- by radiosity, to a hundredth -- so a picture of it says whether
--- an `IrradianceVolume` hands a room more light than its lamp put in.
---
--- The scene data chooses the picture (`run_furnace_gate.cmake`):
---
---   {"volume": true}     the room lit by its lamp and by what its walls
---                        bounce, twice;
---   {"ambient": 0.158}   the same room with no volume, and `Ambient` put to
---                        what two bounces come to at the middle of a wall;
---   {"volume": true, "tint": 0.5}
---                        the volume's light halved by its `Tint`;
---   {"volume": true, "bounceOnly": true, "ambient": 0.1}
---                        the volume adding what bounces to an authored
---                        ambient, which is kept;
---   {"look": "up"}       the ceiling straight above the lamp.
---
--- And for the volume kept in a file (ADR 0207, `run_baked_room_gate.cmake`):
---
---   {"volume": true, "baked": "asset://light/room.irradiance"}
---                        the volume names a file its light is kept in;
---   "spacing": 2         the volume is another volume than the file's;
---   "pillar": true       something stands in the room that did not.

local Lighting = game:GetService("Lighting")
local SceneService = game:GetService("SceneService")

type Asked = {
    volume: boolean?,
    ambient: number?,
    look: string?,
    tint: number?,
    bounceOnly: boolean?,
    baked: string?,
    spacing: number?,
    pillar: boolean?,
}
local asked = (SceneService:GetLoadData() or {}) :: Asked
local ambient = asked.ambient or 0

-- No sun, no sky: the lamp is all the light there is.
Lighting.Brightness = 0
Lighting.EnvironmentDiffuseScale = 0
Lighting.EnvironmentSpecularScale = 0
Lighting.Ambient = Color3.new(ambient, ambient, ambient)
Lighting.OutdoorAmbient = Color3.new(ambient, ambient, ambient)

-- Eight metres a side inside, walls half a metre thick, half of what lands
-- on them given back.
local Half = 4
local Thick = 0.5
local Grey = Color3.new(0.5, 0.5, 0.5)
local function slab(name: string, size: vector, position: vector)
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", Grey)
    part.Anchored = true
    part.Parent = workspace
end
local Outer = Half * 2 + Thick * 2
slab("Floor", vector.create(Outer, Thick, Outer), vector.create(0, -Half - Thick / 2, 0))
slab("Ceiling", vector.create(Outer, Thick, Outer), vector.create(0, Half + Thick / 2, 0))
slab("West", vector.create(Thick, Outer, Outer), vector.create(-Half - Thick / 2, 0, 0))
slab("East", vector.create(Thick, Outer, Outer), vector.create(Half + Thick / 2, 0, 0))
slab("North", vector.create(Outer, Outer, Thick), vector.create(0, 0, -Half - Thick / 2))
slab("South", vector.create(Outer, Outer, Thick), vector.create(0, 0, Half + Thick / 2))

-- The lamp hangs from something that is not there: not drawn, and nothing a
-- ray meets.
local holder = Instance.new("Part")
holder.Name = "Lamp"
holder.Size = vector.create(0.1, 0.1, 0.1)
holder.Position = vector.zero
holder:SetMaterialParameter("Transparency", 1)
holder.Anchored = true
holder.CanCollide = false
holder.CanQuery = false
holder.CastShadow = false
holder.Parent = workspace
local light = Instance.new("PointLight")
light.Brightness = 2
light.Range = 20
light.Parent = holder

if asked.volume then
    local box = Instance.new("Part")
    box.Name = "Room"
    box.Size = vector.create(Half * 2, Half * 2, Half * 2)
    box.Position = vector.zero
    box:SetMaterialParameter("Transparency", 1)
    box.CanCollide = false
    box.CanQuery = false
    box.Anchored = true
    box.Parent = workspace
    local volume = Instance.new("IrradianceVolume")
    volume.Spacing = asked.spacing or 1
    volume.Bounces = 2
    if asked.baked then
        volume.Baked = asked.baked :: Content
    end
    if asked.tint then
        volume.Tint = Color3.new(asked.tint, asked.tint, asked.tint)
    end
    if asked.bounceOnly then
        volume.Mode = Enum.IrradianceMode.BounceOnly
    end
    volume.Parent = box
end

-- Out of the camera's sight, in a corner behind it.
if asked.pillar then
    slab("Pillar", vector.create(0.6, Half * 2, 0.6), vector.create(3.2, 0, 3.6))
end

local camera = Instance.new("Camera")
camera.FieldOfView = 70
camera.Parent = workspace
workspace.CurrentCamera = camera
if asked.look == "up" then
    camera.CFrame = CFrame.lookAt(vector.create(0.01, -3.5, 0.3), vector.create(0, 4, 0))
else
    camera.CFrame = CFrame.lookAt(vector.create(0, 0, 3.5), vector.create(0, 0, -4))
end
