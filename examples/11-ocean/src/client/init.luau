--!strict
--- examples/11-ocean — a sea, a boat that floats on it, and nothing faked.
---
--- **The sea is a `Water`** (ADR 0118): three `WaterWave`s under it are the
--- whole sea state, and the engine draws them and floats things on them from
--- the one function, so a hull sits on the water it is drawn on. This file
--- holds no wave maths at all.
---
--- **The boat is simulated.** Its hull is an ordinary unanchored part of
--- density 0.4; the water holds it up where the weight of what it pushes aside
--- balances its own, and because it is held up at 27 points across its volume
--- rather than at its centre, a wave under one side rolls it and the water
--- rights it again. The script only sails it: a push at the stern for the
--- engine, a twist for the rudder, and a keel that stops it sliding sideways.
--- The mast, the sail and the cabin are welded on, so they go where the hull
--- goes and do not float on their own.
---
--- **The cargo is nothing but parts with a density.** 0.35 rides high like
--- balsa, 0.85 sits waterlogged, past 1 sinks -- the water's own arithmetic.
---
--- **Controls.** W/S throttle, A/D steer, arrow keys or the right stick orbit
--- the camera, Space drops a crate over the side.

local DebugService = game:GetService("DebugService")
local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

-- --- The look -----------------------------------------------------------------

local Horizon = Color3.fromRGB(146, 158, 172)

Lighting.ClockTime = 8.4
Lighting.GeographicLatitude = 22
Lighting.Ambient = Color3.fromRGB(26, 34, 46)
Lighting.OutdoorAmbient = Color3.fromRGB(26, 34, 46)
Lighting.Brightness = 2.6
-- Auto exposure is on by default, and an ocean is a big even field of one
-- brightness -- exactly the scene a metering system lifts until the water is
-- pale. Found by measuring the water's pixels against a render with metering
-- off, not by eye.
Lighting.ExposureCompensation = -0.7
Lighting.FogColor = Horizon
-- The sea runs out at a few hundred metres; the fog closes long before.
Lighting.FogStart = 34
Lighting.FogEnd = 62

-- --- The sea ------------------------------------------------------------------

local Ocean = Instance.new("Water")
Ocean.Name = "Ocean"
Ocean.Shape = Enum.WaterShape.Ocean
Ocean.SurfaceLevel = 0
-- A slow surface current: what makes floating things drift apart into a
-- scatter rather than staying in the line they were dropped in.
Ocean.Current = vector.create(0.45, 0, -0.15)
-- The water's drag grows with speed and with how much of a body is under, not
-- with its shape: at the default a hull doing seven metres a second is held
-- back by nearly its own weight, low on the hull, and noses under. A sea for
-- boats wants it light.
Ocean.Viscosity = 0.4
Ocean.Parent = workspace

--- The sea state. Speed is not in the table: deep-water waves travel at
--- `sqrt(g k)`, so the long swell outruns the short chop on its own.
local WaveDefinitions = {
    -- Wavelength (m), amplitude (m), direction (degrees from +X), how sharp
    -- the crests are, phase.
    { Length = 139, Amplitude = 1.3, Direction = 8.5, Steepness = 0.3, Phase = 0.0 },
    { Length = 67, Amplitude = 0.75, Direction = -41.3, Steepness = 0.2, Phase = 1.7 },
    { Length = 41, Amplitude = 0.4, Direction = 110.4, Steepness = 0.0, Phase = 3.9 },
}

for index, definition in WaveDefinitions do
    local wave = Instance.new("WaterWave")
    wave.Name = `Wave{index}`
    wave.Wavelength = definition.Length
    wave.Amplitude = definition.Amplitude
    wave.Direction = definition.Direction
    wave.Steepness = definition.Steepness
    wave.Phase = definition.Phase
    wave.Parent = Ocean
end

-- --- The boat -----------------------------------------------------------------

local HullColor = Color3.fromRGB(112, 72, 48)
local DeckColor = Color3.fromRGB(182, 148, 102)
local SailColor = Color3.fromRGB(226, 222, 210)

local BoatLength = 9.5
local BoatBeam = 3.2
local BoatDepth = 1.6
--- Under half the water's: it floats with 40% of itself under, and a box that
--- wide for that draft is stiff -- it rights itself, and rolls with the sea
--- rather than past it.
local HullDensity = 0.4
local HullTop = BoatDepth * 0.5

local hull = Instance.new("Part")
hull.Name = "Hull"
hull.Size = vector.create(BoatBeam, BoatDepth, BoatLength)
hull:SetMaterialParameter("Color", HullColor)
hull.Density = HullDensity
hull.CFrame = CFrame.new(0, 0, 0)
hull.Parent = workspace

--- Welds `part` to the hull at `offset`. A welded part is placed by its weld
--- rather than simulated, so it neither floats nor sinks on its own: it rides.
--- Not collidable, so the pieces that overlap the hull do not shove it.
local function boatPiece(name: string, offset: CFrame, size: vector, color: Color3, shape: EnumPartShape): Part
    local part = Instance.new("Part")
    part.Name = name
    part.Shape = shape
    part.Size = size
    part:SetMaterialParameter("Color", color)
    part.CFrame = offset
    part.CanCollide = false
    part.Parent = workspace

    local weld = Instance.new("Weld")
    weld.Part0 = hull
    weld.Part1 = part
    weld.C0 = offset
    weld.Parent = hull
    return part
end

boatPiece(
    "Deck",
    CFrame.new(0, HullTop - 0.05, 0),
    vector.create(BoatBeam - 0.3, 0.2, BoatLength - 0.4),
    DeckColor,
    Enum.PartShape.Block
)
-- The wedge's tall face points at -Z and -Z is forward, so an unrotated one is
-- already a bow. Turning it would give a boat that sails stern first.
boatPiece(
    "Bow",
    CFrame.new(0, 0, -(BoatLength * 0.5 + 1.3)) * CFrame.fromEuler(0, math.pi, 0, Enum.RotationOrder.YXZ),
    vector.create(BoatBeam, BoatDepth, 2.6),
    HullColor,
    Enum.PartShape.Wedge
)
boatPiece("Cabin", CFrame.new(0, HullTop + 0.6, 2.6), vector.create(1.9, 1.2, 2.2), DeckColor, Enum.PartShape.Block)
boatPiece(
    "Mast",
    CFrame.new(0, HullTop + 3.2, -1.2),
    vector.create(0.26, 6.4, 0.26),
    HullColor,
    Enum.PartShape.Cylinder
)
-- A square sail, across the beam: seen from astern, where the chase camera
-- lives, a fore-and-aft one is a sliver one pixel wide.
boatPiece("Sail", CFrame.new(0, HullTop + 2.8, -0.55), vector.create(3.0, 4.0, 0.12), SailColor, Enum.PartShape.Block)
boatPiece("Yard", CFrame.new(0, HullTop + 4.85, -1.2), vector.create(3.6, 0.16, 0.16), HullColor, Enum.PartShape.Block)

--- What the engine and the rudder push: the hull's mass, and how hard it is to
--- turn about the vertical.
local HullMass = HullDensity * BoatBeam * BoatDepth * BoatLength
local HullYawInertia = HullMass * (BoatBeam * BoatBeam + BoatLength * BoatLength) / 12

local BoatTopSpeed = 7
local BoatTurnRate = 0.6
--- How hard the rudder chases the turn it is asked for, per second.
local RudderGrip = 2.5
--- How much of the sideways slide the keel takes out, per second.
local KeelGrip = 3.0

-- --- The cargo ----------------------------------------------------------------

local flotsam = Instance.new("Folder")
flotsam.Name = "Flotsam"
flotsam.Parent = workspace

local function addFloater(
    name: string,
    position: vector,
    size: vector,
    density: number,
    color: Color3,
    shape: EnumPartShape
)
    local part = Instance.new("Part")
    part.Name = name
    part.Shape = shape
    part.Size = size
    part:SetMaterialParameter("Color", color)
    part.Density = density
    part.CFrame = CFrame.new(position)
    part.Parent = flotsam
end

local CrateColor = Color3.fromRGB(186, 140, 82)
local BarrelColor = Color3.fromRGB(138, 96, 62)

for index = 0, 7 do
    local angle = index * 0.7854
    local radius = 14 + index * 2.5
    addFloater(
        `Crate{index}`,
        vector.create(math.cos(angle) * radius, 3, math.sin(angle) * radius),
        vector.create(1.6, 1.6, 1.6),
        -- Spread across the interesting range on purpose, so the difference is
        -- visible from the deck.
        0.35 + index * 0.07,
        CrateColor,
        Enum.PartShape.Block
    )
end

for index = 0, 3 do
    addFloater(
        `Barrel{index}`,
        vector.create(-10 - index * 4, 3, 8 + index * 3),
        vector.create(1.3, 1.9, 1.3),
        0.48,
        BarrelColor,
        Enum.PartShape.Cylinder
    )
end

-- --- The camera ---------------------------------------------------------------

local camera = Instance.new("Camera")
camera.Name = "ChaseCamera"
camera.FieldOfView = 68
camera.NearPlane = 0.15
camera.FarPlane = 900
camera.Parent = workspace
workspace.CurrentCamera = camera

local CameraDistance = 17
local CameraTurnSpeed = 2.0

-- --- Input --------------------------------------------------------------------

-- Sailing is a decision, so it is on the simulation clock where the replay can
-- see it (ADR 0039). The camera is not, so it is not.
local Gameplay = Instance.new("InputContext")
Gameplay.Name = "Gameplay"
Gameplay.Parent = workspace

local SailAction = Instance.new("InputAction")
SailAction.Name = "Sail"
SailAction.Type = Enum.InputActionType.Direction2D
SailAction.Parent = Gameplay

local SailKeys = Instance.new("InputBinding")
SailKeys.Up = Enum.KeyCode.W
SailKeys.Down = Enum.KeyCode.S
SailKeys.Left = Enum.KeyCode.A
SailKeys.Right = Enum.KeyCode.D
SailKeys.DisplayName = "WASD"
SailKeys.Parent = SailAction

local SailStick = Instance.new("InputBinding")
SailStick.KeyCode = Enum.KeyCode.LeftThumbstick
SailStick.Parent = SailAction

local DropAction = Instance.new("InputAction")
DropAction.Name = "DropCrate"
DropAction.Type = Enum.InputActionType.Bool
DropAction.Parent = Gameplay

local DropKey = Instance.new("InputBinding")
DropKey.KeyCode = Enum.KeyCode.Space
DropKey.DisplayName = "Space"
DropKey.Parent = DropAction

local DropButton = Instance.new("InputBinding")
DropButton.KeyCode = Enum.KeyCode.ButtonSouth
DropButton.Parent = DropAction

local CameraControls = Instance.new("InputContext")
CameraControls.Name = "CameraControls"
CameraControls.Rate = Enum.InputRate.Render
CameraControls.Parent = workspace

local OrbitAction = Instance.new("InputAction")
OrbitAction.Name = "Orbit"
OrbitAction.Type = Enum.InputActionType.Direction2D
OrbitAction.Parent = CameraControls

local OrbitKeys = Instance.new("InputBinding")
OrbitKeys.Up = Enum.KeyCode.Up
OrbitKeys.Down = Enum.KeyCode.Down
OrbitKeys.Left = Enum.KeyCode.Left
OrbitKeys.Right = Enum.KeyCode.Right
OrbitKeys.Parent = OrbitAction

local OrbitStick = Instance.new("InputBinding")
OrbitStick.KeyCode = Enum.KeyCode.RightThumbstick
OrbitStick.Parent = OrbitAction

-- --- State --------------------------------------------------------------------

local cameraYaw = 0
local cameraHeight = 8

local droppedCrates = 0
local dropHeld = false

-- --- The tick -----------------------------------------------------------------

RunService.Heartbeat:Connect(function(dt: number)
    local frame = hull.CFrame
    local position = frame.Position

    -- The heading, level: the way the bow points with the roll and the pitch
    -- taken out, which is the way the boat is going.
    local look = frame.LookVector
    local forward = vector.normalize(vector.create(look.x, 0, look.z))
    local right = vector.create(-forward.z, 0, forward.x)

    -- --- Sailing ---------------------------------------------------------------

    -- Only a hull in the water is sailed: a screw in the air pushes nothing.
    local surface = Ocean:GetHeightAt(position) or 0
    local afloat = position.y - BoatDepth * 0.5 < surface

    local sail = SailAction:GetState() :: Vector2
    local velocity = hull.LinearVelocity
    local speed = vector.dot(velocity, forward)
    if afloat then
        -- The screw: at the stern, low, so a hard throttle lifts the bow the
        -- way it does on a real boat. The water's own drag is what caps the
        -- speed -- it grows with `Viscosity` -- and this is sized so that the
        -- cap is `BoatTopSpeed`.
        local thrust = forward * (sail.Y * BoatTopSpeed * HullMass * Ocean.Viscosity)
        local stern = frame:PointToWorldSpace(vector.create(0, -BoatDepth * 0.3, BoatLength * 0.5))
        hull:ApplyImpulseAtPosition(thrust * dt, stern)

        -- The keel: a hull slides forward easily and sideways hardly at all.
        local slide = vector.dot(velocity, right)
        hull:ApplyImpulse(right * (-slide * HullMass * math.min(1, KeelGrip * dt)))

        -- The rudder: a turn only as fast as the boat is moving, which is the
        -- one thing that makes a boat feel like a boat rather than like a car.
        local wanted = -sail.X * BoatTurnRate * math.clamp(speed / BoatTopSpeed, -1, 1)
        local turning = hull.AngularVelocity.y
        hull:ApplyAngularImpulse(
            vector.create(0, (wanted - turning) * HullYawInertia * math.min(1, RudderGrip * dt), 0)
        )
    end

    -- --- Dropping cargo --------------------------------------------------------

    local dropWanted = DropAction:GetState() :: boolean
    if dropWanted and not dropHeld and droppedCrates < 24 then
        droppedCrates += 1
        local drop = position + right * (BoatBeam * 0.5 + 1.2) + vector.create(0, 3, 0)
        addFloater(
            `Dropped{droppedCrates}`,
            drop,
            vector.create(1.4, 1.4, 1.4),
            0.42,
            Color3.fromRGB(206, 118, 74),
            Enum.PartShape.Block
        )
    end
    dropHeld = dropWanted

    -- --- The camera ------------------------------------------------------------

    local orbit = OrbitAction:GetState() :: Vector2
    cameraYaw += CameraTurnSpeed * orbit.X * dt
    cameraHeight = math.clamp(cameraHeight + 7 * orbit.Y * dt, 2.5, 26)

    -- Behind the boat's HEADING, not behind its hull: chasing the hull would
    -- put the camera through every wave the boat rolls into.
    local heading = math.atan2(-forward.x, -forward.z)
    local chase = vector.create(-math.sin(heading + cameraYaw), 0, -math.cos(heading + cameraYaw))
    local focus = position + vector.create(0, 2.2, 0)
    local target = focus - chase * CameraDistance + vector.create(0, cameraHeight, 0)

    -- Held above the water whatever the orbit asks for, so the camera never
    -- ends up inside the sea.
    local floor = (Ocean:GetHeightAt(target) or 0) + 1.4
    if target.y < floor then
        target = vector.create(target.x, floor, target.z)
    end

    local eye = camera.CFrame.Position
    eye += (target - eye) * math.min(1, 5 * dt)
    camera.CFrame = CFrame.lookAt(eye, focus)
end)

DebugService:ShowPanel("Stats")

print("11-ocean: W/S throttle, A/D steer, arrows orbit, Space drops a crate over the side")
