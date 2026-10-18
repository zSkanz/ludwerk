--!strict
--- `@engine/xrcomfort` -- moving in a headset without being sick (ADR 0246 §3).
---
--- A player who walks with a stick while standing still is the main cause of
--- discomfort in VR, and every VR game offers the same answers: a teleport, a
--- snap turn and a vignette. This module is those answers, so a game made
--- with the engine does not write them first. It is a module a game requires,
--- on `XRService` and the camera; the engine's core gains nothing for it.
---
--- **It owns the camera's `CFrame` while it runs.** In a headset the camera is
--- where the play space is -- the floor under the player -- and the head moves
--- the view from there (ADR 0245). The session keeps that place as `Origin`;
--- a game that moves its player somewhere sets `Origin`, and every frame the
--- session writes the camera from it, raised by the player's height offset.
---
--- - **Teleport** (`Move = "teleport"`): the left stick pushed forward draws
---   an arc from the left hand to where it lands. Released, the view fades
---   for a tenth of a second and the play space moves there -- only onto a
---   surface flat enough to stand on, never onto a wall.
--- - **Smooth move** (`Move = "smooth"`): the left stick walks the play space
---   the way the head faces, and the edges of the view darken while it moves
---   (`Vignette`, how much of the view they close; 0 for none).
--- - **Snap turn** (`Turn = "snap"`): the right stick pushed to a side turns
---   the play space by `SnapDegrees` about the head, in one frame.
--- - **Smooth turn** (`Turn = "smooth"`): at `TurnRate` degrees a second.
--- - **Seated**: the play space is raised so a player sitting has a standing
---   player's eyes; `CalibrateHeight` makes where the head is now that height.
---
--- Flat, with no headset, `start` returns a session that does nothing.
---
--- Casing follows ADR 0034: `xrcomfort.start` is a module function; what it
--- returns is an object, so `session:Stop()`.

local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local XrComfort = {}

export type Options = {
    --- `"teleport"` (the default) or `"smooth"`.
    Move: string?,
    --- `"snap"` (the default) or `"smooth"`.
    Turn: string?,
    --- Degrees a snap turn turns by. 30 by default.
    SnapDegrees: number?,
    --- Degrees a second a smooth turn turns at, the stick held over. 90 by
    --- default.
    TurnRate: number?,
    --- Metres a second a smooth move walks at, the stick held forward. 3 by
    --- default.
    Speed: number?,
    --- How much of the view the vignette closes at full speed, 0 to 1: 0.6 by
    --- default, 0 for none.
    Vignette: number?,
    --- A seated player is raised to a standing player's eyes.
    Seated: boolean?,
    --- Metres added to the player's height. `CalibrateHeight` sets it.
    HeightOffset: number?,
    --- The camera it moves; `workspace.CurrentCamera` by default.
    Camera: Camera?,
}

export type Session = {
    --- Where the play space is in the world, before the height offset: the
    --- floor under the player. A game moves its player by setting it.
    Origin: CFrame,
    HeightOffset: number,
    --- Whether the vignette is closed, 0 open to 1 shut, this frame: for a
    --- game that wants to show it its own way.
    VignetteAmount: number,
    --- Makes where the head is now a standing player's eye height.
    CalibrateHeight: (self: Session) -> (),
    --- Steps the session once, when no frame is drawn: call on `Heartbeat`.
    --- Where a frame is drawn the session's render step moves it instead,
    --- and this does nothing.
    Update: (self: Session, dt: number) -> (),
    --- Stops it: the camera stays where it is, and the arc and the vignette
    --- are taken away.
    Stop: (self: Session) -> (),
}

-- A standing player's eyes, in metres over the floor.
local StandingEyes = 1.6
-- How far a stick goes before it counts, and how far back it must come.
local StickOn = 0.7
local StickOff = 0.3
-- The teleport's arc: how fast it is thrown, how far it falls a second, and
-- how much of it is followed.
local ArcSpeed = 8
local ArcGravity = 9.8
local ArcStep = 0.05
local ArcSteps = 40
-- How upright a surface must be to stand on: the cosine of 45 degrees.
local Walkable = 0.7
-- How long a teleport's fade is, in seconds.
local FadeSeconds = 0.1

-- How many sessions this VM has made: each binds its render step under its
-- own name.
local Sessions = 0

local function yawOf(cframe: CFrame): number
    local look = cframe.LookVector
    return math.atan2(-look.x, -look.z)
end

-- Turns `origin` by `angle` radians about the vertical through `pivot`.
local function turnAbout(origin: CFrame, pivot: vector, angle: number): CFrame
    local about = CFrame.new(pivot.x, origin.Position.y, pivot.z)
    return about * CFrame.fromAxisAngle(vector.create(0, 1, 0), angle) * about:Inverse() * origin
end

local function makeStick(context: InputContext, name: string, key: EnumKeyCode): InputAction
    local action = Instance.new("InputAction")
    action.Name = name
    action.Type = Enum.InputActionType.Direction2D
    action.Parent = context
    local binding = Instance.new("InputBinding")
    binding.KeyCode = key
    binding.Parent = action
    return action
end

-- The vignette: a frame on a surface a hand's width in front of the eyes,
-- clear in the middle and dark at the edges, laid over everything.
local function makeVignette(): (BasePart, Frame)
    local plate = Instance.new("Part")
    plate.Name = "XRComfortVignette"
    plate.Anchored = true
    plate.CanCollide = false
    plate.CanQuery = false
    plate.CanTouch = false
    -- Drawn only by its interface, never as a part.
    plate.Fade = 1
    plate.Size = vector.create(1, 1, 0.01)
    local surface = Instance.new("SurfaceGui")
    surface.Face = Enum.Face.Back
    surface.AlwaysOnTop = true
    surface.PixelsPerMeter = 400
    surface.Parent = plate
    local shade = Instance.new("Frame")
    shade.Size = UDim2.fromScale(1, 1)
    shade.BackgroundColor = Color3.fromRGB(0, 0, 0)
    shade.BackgroundTransparency = 1
    shade.Parent = surface
    local gradient = Instance.new("UIGradient")
    gradient.Type = Enum.GradientType.Radial
    gradient.Transparency = NumberSequence.new({
        NumberSequenceKeypoint.new(0, 1),
        NumberSequenceKeypoint.new(0.55, 1),
        NumberSequenceKeypoint.new(1, 0),
    })
    gradient.Parent = shade
    return plate, shade
end

-- The teleport's arc: a beam between two points on an invisible anchor.
local function makeArc(): (BasePart, Attachment, Attachment, Beam)
    local anchor = Instance.new("Part")
    anchor.Name = "XRComfortArc"
    anchor.Anchored = true
    anchor.CanCollide = false
    anchor.CanQuery = false
    anchor.CanTouch = false
    anchor.Fade = 1
    anchor.Size = vector.create(0.1, 0.1, 0.1)
    local from = Instance.new("Attachment")
    from.Parent = anchor
    local to = Instance.new("Attachment")
    to.Parent = anchor
    local beam = Instance.new("Beam")
    beam.Attachment0 = from
    beam.Attachment1 = to
    beam.Width0 = 0.02
    beam.Width1 = 0.02
    beam.FaceCamera = true
    beam.Enabled = false
    beam.Parent = anchor
    return anchor, from, to, beam
end

--- Starts moving the player in a headset with `options`.
function XrComfort.start(options: Options?): Session
    local given: Options = options or {}
    local camera = given.Camera or workspace.CurrentCamera
    local moveKind = given.Move or "teleport"
    local turnKind = given.Turn or "snap"
    local snapDegrees = given.SnapDegrees or 30
    local turnRate = given.TurnRate or 90
    local speed = given.Speed or 3
    local vignetteMost = math.clamp(given.Vignette or 0.6, 0, 1)
    local seated = given.Seated == true

    local self = {
        Origin = if camera then camera.CFrame else CFrame.new(),
        HeightOffset = given.HeightOffset or 0,
        VignetteAmount = 0,
    }

    local context = Instance.new("InputContext")
    context.Name = "XRComfort"
    context.Rate = Enum.InputRate.Render
    context.Parent = workspace
    local moveStick = makeStick(context, "XRComfortMove", Enum.KeyCode.XRThumbstickLeft)
    local turnStick = makeStick(context, "XRComfortTurn", Enum.KeyCode.XRThumbstickRight)

    local plate, shade = makeVignette()
    local arcAnchor, arcFrom, arcTo, arcBeam = makeArc()
    local parented = false

    local snapped = false
    local aiming = false
    local target: vector? = nil
    local fading = 0
    local stepped = false
    local name: string? = nil

    local function lift(): CFrame
        local raise = self.HeightOffset
        if seated then
            raise += StandingEyes
        end
        return CFrame.new(0, raise, 0)
    end

    -- Where the head is in the world this frame.
    local function headInWorld(): CFrame
        return self.Origin * lift() * XRService:GetHeadCFrame()
    end

    -- Follows the arc from the left hand; where it lands, if on something
    -- flat enough to stand on.
    local function landing(): (vector?, vector?)
        local from, along = XRService:GetPointerRay(Enum.XRHand.Left)
        if not from or not along then
            return nil, nil
        end
        local params = RaycastParams.new({ Filter = { plate, arcAnchor }, FilterType = Enum.RaycastFilterType.Exclude })
        local at = from
        local velocity = along * ArcSpeed
        for _ = 1, ArcSteps do
            local nextVelocity = velocity + vector.create(0, -ArcGravity * ArcStep, 0)
            local move = (velocity + nextVelocity) * (ArcStep * 0.5)
            local hit = workspace:Raycast(at, move, params)
            if hit then
                if hit.Normal.y >= Walkable then
                    return from, hit.Position
                end
                return from, nil
            end
            at += move
            velocity = nextVelocity
        end
        return from, nil
    end

    local function step(dt: number)
        if not XRService.Enabled or camera == nil then
            return
        end
        if not parented then
            parented = true
            plate.Parent = workspace
            arcAnchor.Parent = workspace
        end
        local moveValue = moveStick:GetState() :: Vector2
        local turnValue = turnStick:GetState() :: Vector2
        local head = headInWorld()

        -- **Turning**, about the head: the play space turns under the player.
        if turnKind == "snap" then
            if not snapped and math.abs(turnValue.X) >= StickOn then
                snapped = true
                local angle = math.rad(snapDegrees) * (if turnValue.X > 0 then -1 else 1)
                self.Origin = turnAbout(self.Origin, head.Position, angle)
            elseif snapped and math.abs(turnValue.X) <= StickOff then
                snapped = false
            end
        elseif math.abs(turnValue.X) > 0.1 then
            self.Origin = turnAbout(self.Origin, head.Position, -math.rad(turnRate) * turnValue.X * dt)
        end

        -- **Moving**.
        local moving = 0
        if moveKind == "smooth" then
            local magnitude = math.min(1, math.sqrt(moveValue.X * moveValue.X + moveValue.Y * moveValue.Y))
            if magnitude > 0.1 then
                local yaw = yawOf(head)
                local forward = vector.create(-math.sin(yaw), 0, -math.cos(yaw))
                local right = vector.create(math.cos(yaw), 0, -math.sin(yaw))
                local walk = (forward * moveValue.Y + right * moveValue.X) * (speed * dt)
                self.Origin = CFrame.new(walk.x, walk.y, walk.z) * self.Origin
                moving = magnitude
            end
        else
            if moveValue.Y >= StickOn then
                aiming = true
                local from, lands = landing()
                target = lands
                if from then
                    arcFrom.CFrame = CFrame.new(from.x, from.y, from.z)
                    local ends = lands or (from + vector.create(0, -1, 0))
                    arcTo.CFrame = CFrame.new(ends.x, ends.y, ends.z)
                    arcBeam.Enabled = true
                end
            elseif aiming and moveValue.Y <= StickOff then
                aiming = false
                arcBeam.Enabled = false
                if target then
                    -- The play space moves so the head stands over the target.
                    local shift = target - vector.create(head.Position.x, self.Origin.Position.y, head.Position.z)
                    self.Origin = CFrame.new(shift.x, shift.y, shift.z) * self.Origin
                    fading = FadeSeconds
                end
                target = nil
            end
        end

        -- **The vignette**: closed in proportion to how fast the view moves,
        -- and shut while a teleport fades.
        local amount = vignetteMost * moving
        if fading > 0 then
            fading -= dt
            amount = 1
        end
        self.VignetteAmount = amount
        shade.BackgroundTransparency = 1 - amount
        plate.CFrame = head * CFrame.new(0, 0, -0.15)

        local lens = camera :: Camera
        lens.CFrame = self.Origin * lift()
    end

    Sessions += 1
    name = `XRComfort{Sessions}`
    RunService:BindToRenderStep(name :: string, Enum.RenderPriority.Camera.Value, function(dt: number)
        stepped = true
        step(dt)
    end)

    local session = self :: any
    function session.CalibrateHeight(_: Session)
        local head = XRService:GetHeadCFrame()
        self.HeightOffset = StandingEyes - head.Position.y - (if seated then StandingEyes else 0)
    end
    function session.Update(_: Session, dt: number)
        if stepped then
            stepped = false
            return
        end
        step(dt)
    end
    function session.Stop(_: Session)
        if name then
            RunService:UnbindFromRenderStep(name)
            name = nil
        end
        context:Destroy()
        plate:Destroy()
        arcAnchor:Destroy()
    end
    return session :: Session
end

return XrComfort
