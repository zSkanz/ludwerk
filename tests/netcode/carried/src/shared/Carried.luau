--!strict
--[[
	What a spawned thing carries, and how each end says what it holds.

	`fill` puts under a part one of each class protocol 44 sends, authored away
	from its defaults -- the stamp in `content/stamps` is the same thing written
	as a file. `describe` is what an end reads back of them: a line a class, the
	same text on both ends when the values are the same, the sequences key by
	key. `change` is one property of each, written by the server once the
	joiner has said what arrived.

	Two of the "classes" are the thing itself: `Body`, the part and what was
	authored on it -- how it grips, what it weighs, whether a ray finds it,
	asked with a ray -- and `Rig`, a model under it with a primary part and a
	pivot. And one line is the world's: the light under the open sky.
]]

local Lighting = game:GetService("Lighting")
local Workspace = game:GetService("Workspace")

local Carried = {}

--- The classes, in the order both ends speak of them.
Carried.Classes = {
    "PointLight",
    "SpotLight",
    "SpringBone",
    "SpringCollider",
    "Bone",
    "Highlight",
    "Trail",
    "Beam",
    "ParticleEmitter",
    "Sound",
    "Body",
    "Rig",
}

--- What both ends read of the world itself, keyed `World.<name>`.
Carried.World = { "Lighting" }

--- The three ways the server makes one, which is a thing's name before the
--- player's number.
Carried.Ways = { "Stamped", "Cloned", "Made" }

local function fire(): ColorSequence
    return ColorSequence.new({
        ColorSequenceKeypoint.new(0, Color3.new(1, 0.9, 0.5)),
        ColorSequenceKeypoint.new(0.25, Color3.new(1, 0.5, 0.125)),
        ColorSequenceKeypoint.new(1, Color3.new(0.25, 0, 0)),
    })
end

local function fade(): NumberSequence
    return NumberSequence.new({
        NumberSequenceKeypoint.new(0, 0),
        NumberSequenceKeypoint.new(0.75, 0.25, 0.125),
        NumberSequenceKeypoint.new(1, 1),
    })
end

local function swell(): NumberSequence
    return NumberSequence.new({
        NumberSequenceKeypoint.new(0, 0.5),
        NumberSequenceKeypoint.new(0.5, 2),
        NumberSequenceKeypoint.new(1, 0.25),
    })
end

--- One of each class under `body`, authored -- and the body's own.
function Carried.fill(body: BasePart)
    body.Friction = 0.875
    body.Restitution = 0.5
    body.Density = 4
    body.LinearDamping = 0.25
    body.AngularDamping = 0.5
    body.Buoyant = false
    body.CanTouch = false
    body.CanQuery = false
    body.ContactDetails = true
    body.PivotOffset = CFrame.new(0, -1, 0)

    -- Its handle is out of every ray's way: what is asked of a ray is the body.
    local rig = Instance.new("Model")
    rig.Name = "Rig"
    local handle = Instance.new("Part")
    handle.Name = "Handle"
    handle.Anchored = true
    handle.CanCollide = false
    handle.CanQuery = false
    handle.Size = vector.create(0.25, 0.25, 0.25)
    handle.CFrame = CFrame.new(0, 40, 0)
    handle.Parent = rig
    rig.PrimaryPart = handle
    rig.PivotOffset = CFrame.new(0, 0.5, 0)
    rig.Parent = body

    local a0 = Instance.new("Attachment")
    a0.Name = "A0"
    a0.CFrame = CFrame.new(0, 1, 0)
    a0.Parent = body
    local a1 = Instance.new("Attachment")
    a1.Name = "A1"
    a1.CFrame = CFrame.new(0, -1, 0)
    a1.Parent = body

    local point = Instance.new("PointLight")
    point.CFrame = CFrame.new(0, 0.5, 0)
    point.Color = Color3.new(1, 0.75, 0.5)
    point.Brightness = 3
    point.Range = 12
    point.Shadows = true
    point.Parent = body

    local spot = Instance.new("SpotLight")
    spot.CFrame = CFrame.new(0, 0.25, -0.5)
    spot.Color = Color3.new(0.5, 0.75, 1)
    spot.Brightness = 2
    spot.Range = 24
    spot.Angle = 30
    spot.Enabled = false
    spot.Parent = body

    local spring = Instance.new("SpringBone")
    spring.RootJoint = "Cape1"
    spring.JointPattern = "Cape*"
    spring.Stiffness = 0.5
    spring.Damping = 0.25
    spring.GravityScale = 0.75
    spring.LimitAngle = 45
    spring.Parent = body

    local collider = Instance.new("SpringCollider")
    collider.JointName = "Spine"
    collider.Radius = 0.375
    collider.Length = 0.5
    collider.Offset = vector.create(0, 0.125, 0)
    collider.Parent = body

    local bone = Instance.new("Bone")
    bone.JointName = "Head"
    bone.CFrame = CFrame.new(0, 0.75, 0)
    bone.Transform = CFrame.new(0, 0.25, 0)
    bone.Parent = body

    local highlight = Instance.new("Highlight")
    highlight.Adornee = body
    highlight.FillColor = Color3.new(0.25, 1, 0.25)
    highlight.FillTransparency = 0.75
    highlight.OutlineColor = Color3.new(0, 0.5, 0)
    highlight.OutlineTransparency = 0.25
    highlight.DepthMode = Enum.HighlightDepthMode.Occluded
    highlight.Parent = body

    local trail = Instance.new("Trail")
    trail.Attachment0 = a0
    trail.Attachment1 = a1
    trail.Lifetime = 1.5
    trail.Color = fire()
    trail.Transparency = fade()
    trail.WidthScale = swell()
    trail.Texture = "asset://textures/trail.png"
    trail.Parent = body

    local beam = Instance.new("Beam")
    beam.Attachment0 = a0
    beam.Attachment1 = a1
    beam.Color = fire()
    beam.Transparency = fade()
    beam.Width0 = 0.5
    beam.Width1 = 0.25
    beam.Segments = 16
    beam.TextureMode = Enum.TextureMode.Static
    beam.Parent = body

    local emitter = Instance.new("ParticleEmitter")
    emitter.Rate = 40
    emitter.Texture = "asset://textures/flame.png"
    emitter.FlipbookColumns = 4
    emitter.FlipbookRows = 2
    emitter.FlipbookMode = Enum.ParticleFlipbookMode.OverLife
    emitter.RotationSpeed = 90
    emitter.ColorOverLife = fire()
    emitter.SizeOverLife = swell()
    emitter.TransparencyOverLife = fade()
    emitter.Collision = Enum.ParticleCollision.Terrain
    emitter.Bounce = 0.25
    emitter.Parent = body

    local sound = Instance.new("Sound")
    sound.Content = "asset://sounds/torch.ogg"
    sound.Looped = true
    sound.Volume = 0.25
    sound.PlaybackSpeed = 1.5
    sound.RollOffMaxDistance = 40
    sound.Playing = true
    sound.Parent = body
end

--- The world's own, as the server sets it at the start and changes it after.
function Carried.light(changed: boolean)
    Lighting.OutdoorAmbient = if changed then Color3.new(0.125, 0.25, 0.5) else Color3.new(0.5, 0.25, 0.125)
end

--- One property of each, as the server changes it afterwards.
function Carried.change(body: Instance)
    local part = body :: BasePart
    part.Friction = 0.125
    part.CanQuery = true
    local rig = body:FindFirstChild("Rig") :: Model
    rig.PivotOffset = CFrame.new(0, 2, 0);
    (body:FindFirstChild("PointLight") :: PointLight).Brightness = 7
    (body:FindFirstChild("SpotLight") :: SpotLight).Enabled = true
    (body:FindFirstChild("SpringBone") :: SpringBone).Stiffness = 0.875
    (body:FindFirstChild("SpringCollider") :: SpringCollider).Radius = 0.625
    (body:FindFirstChild("Bone") :: Bone).Transform = CFrame.new(0, 0.5, 0.125);
    (body:FindFirstChild("Highlight") :: Highlight).FillColor = Color3.new(1, 0.25, 0.25);
    (body:FindFirstChild("Trail") :: Trail).WidthScale = NumberSequence.new({
        NumberSequenceKeypoint.new(0, 1),
        NumberSequenceKeypoint.new(0.125, 3, 0.5),
        NumberSequenceKeypoint.new(0.5, 2),
        NumberSequenceKeypoint.new(1, 0),
    });
    (body:FindFirstChild("Beam") :: Beam).Color = ColorSequence.new(Color3.new(0, 0.5, 1), Color3.new(1, 1, 1));
    (body:FindFirstChild("ParticleEmitter") :: ParticleEmitter).ColorOverLife = ColorSequence.new({
        ColorSequenceKeypoint.new(0, Color3.new(0.5, 0.5, 1)),
        ColorSequenceKeypoint.new(0.5, Color3.new(0.25, 0.25, 1)),
        ColorSequenceKeypoint.new(0.5, Color3.new(1, 1, 1)),
        ColorSequenceKeypoint.new(1, Color3.new(0, 0, 0.5)),
    });
    (body:FindFirstChild("Sound") :: Sound).Playing = false
end

local function number(value: number): string
    return string.format("%.4f", value)
end

local function colour(value: Color3): string
    return `{number(value.R)},{number(value.G)},{number(value.B)}`
end

local function place(value: CFrame): string
    local at = value.Position
    return `{number(at.x)},{number(at.y)},{number(at.z)}`
end

local function colours(sequence: ColorSequence): string
    local keys = {}
    for _, key in sequence.Keypoints do
        table.insert(keys, `{number(key.Time)}:{colour(key.Value)}`)
    end
    return table.concat(keys, "/")
end

local function numbers(sequence: NumberSequence): string
    local keys = {}
    for _, key in sequence.Keypoints do
        table.insert(keys, `{number(key.Time)}:{number(key.Value)}~{number(key.Envelope)}`)
    end
    return table.concat(keys, "/")
end

local function named(instance: Instance?): string
    return if instance then instance.Name else "none"
end

--- What this end reads of the world under `name`.
function Carried.describeWorld(name: string): string?
    if name == "Lighting" then
        return `OutdoorAmbient={colour(Lighting.OutdoorAmbient)}`
    end
    return nil
end

-- The body itself: what was authored on it, and what a ray dropped on it from
-- above finds -- nothing while `CanQuery` is off, the body once it is on.
local function describeBody(body: BasePart): string
    local hit = Workspace:Raycast(body.Position + vector.create(0, 20, 0), vector.create(0, -40, 0))
    return `Friction={number(body.Friction)} Restitution={number(body.Restitution)} Density={number(body.Density)} `
        .. `LinearDamping={number(body.LinearDamping)} AngularDamping={number(body.AngularDamping)} `
        .. `Buoyant={body.Buoyant} CanTouch={body.CanTouch} CanQuery={body.CanQuery} `
        .. `ContactDetails={body.ContactDetails} PivotOffset={place(body.PivotOffset)} `
        .. `Ray={if hit then hit.Instance.Name else "miss"}`
end

--- What this end holds of `class` under `body`, as one line: nil when the
--- instance is not there.
function Carried.describe(body: Instance, class: string): string?
    if class == "Body" then
        return if body:IsA("BasePart") then describeBody(body :: BasePart) else nil
    elseif class == "Rig" then
        local rig = body:FindFirstChild("Rig")
        if rig == nil or not rig:IsA("Model") then
            return nil
        end
        local model = rig :: Model
        return `PrimaryPart={named(model.PrimaryPart)} PivotOffset={place(model.PivotOffset)} `
            .. `Pivot={place(model:GetPivot())}`
    end
    local found = body:FindFirstChild(class)
    if found == nil or not found:IsA(class) then
        return nil
    end
    if class == "PointLight" then
        local light = found :: PointLight
        return `CFrame={place(light.CFrame)} Color={colour(light.Color)} Brightness={number(light.Brightness)} `
            .. `Range={number(light.Range)} Enabled={light.Enabled} Shadows={light.Shadows}`
    elseif class == "SpotLight" then
        local light = found :: SpotLight
        return `CFrame={place(light.CFrame)} Color={colour(light.Color)} Brightness={number(light.Brightness)} `
            .. `Range={number(light.Range)} Angle={number(light.Angle)} Enabled={light.Enabled}`
    elseif class == "SpringBone" then
        local spring = found :: SpringBone
        return `RootJoint={spring.RootJoint} JointPattern={spring.JointPattern} `
            .. `Stiffness={number(spring.Stiffness)} Damping={number(spring.Damping)} `
            .. `GravityScale={number(spring.GravityScale)} LimitAngle={number(spring.LimitAngle)} `
            .. `Enabled={spring.Enabled}`
    elseif class == "SpringCollider" then
        local collider = found :: SpringCollider
        local offset = collider.Offset
        return `JointName={collider.JointName} Radius={number(collider.Radius)} Length={number(collider.Length)} `
            .. `Offset={number(offset.x)},{number(offset.y)},{number(offset.z)}`
    elseif class == "Bone" then
        local bone = found :: Bone
        return `JointName={bone.JointName} CFrame={place(bone.CFrame)} Transform={place(bone.Transform)}`
    elseif class == "Highlight" then
        local highlight = found :: Highlight
        return `Adornee={named(highlight.Adornee)} FillColor={colour(highlight.FillColor)} `
            .. `FillTransparency={number(highlight.FillTransparency)} OutlineColor={colour(highlight.OutlineColor)} `
            .. `OutlineTransparency={number(highlight.OutlineTransparency)} DepthMode={highlight.DepthMode.Name} `
            .. `Enabled={highlight.Enabled}`
    elseif class == "Trail" then
        local trail = found :: Trail
        return `Attachment0={named(trail.Attachment0)} Attachment1={named(trail.Attachment1)} `
            .. `Lifetime={number(trail.Lifetime)} Texture={trail.Texture} Color={colours(trail.Color)} `
            .. `Transparency={numbers(trail.Transparency)} WidthScale={numbers(trail.WidthScale)}`
    elseif class == "Beam" then
        local beam = found :: Beam
        return `Attachment0={named(beam.Attachment0)} Attachment1={named(beam.Attachment1)} `
            .. `Width0={number(beam.Width0)} Width1={number(beam.Width1)} Segments={beam.Segments} `
            .. `TextureMode={beam.TextureMode.Name} Color={colours(beam.Color)} `
            .. `Transparency={numbers(beam.Transparency)}`
    elseif class == "ParticleEmitter" then
        local emitter = found :: ParticleEmitter
        return `Rate={number(emitter.Rate)} Texture={emitter.Texture} `
            .. `Flipbook={emitter.FlipbookColumns}x{emitter.FlipbookRows} FlipbookMode={emitter.FlipbookMode.Name} `
            .. `RotationSpeed={number(emitter.RotationSpeed)} Collision={emitter.Collision.Name} `
            .. `Bounce={number(emitter.Bounce)} ColorOverLife={colours(emitter.ColorOverLife)} `
            .. `SizeOverLife={numbers(emitter.SizeOverLife)} `
            .. `TransparencyOverLife={numbers(emitter.TransparencyOverLife)}`
    elseif class == "Sound" then
        local sound = found :: Sound
        return `Content={sound.Content} Playing={sound.Playing} Looped={sound.Looped} `
            .. `Volume={number(sound.Volume)} PlaybackSpeed={number(sound.PlaybackSpeed)} `
            .. `RollOffMaxDistance={number(sound.RollOffMaxDistance)}`
    end
    return nil
end

return Carried
