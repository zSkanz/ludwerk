--!strict
--- `@engine/camera` — the camera rigs (api-design.md §1.1, divergence 16).
---
--- There is no `CameraType` state machine here and no hidden controller: a
--- `Camera` is a scriptable Instance with a `CFrame`, and this is the rig code
--- that would otherwise be copied into every project. Three examples in this
--- repository hand-rolled the same forty lines before it existed.
---
--- **A rig moves its camera every drawn frame, by itself** (ADR 0136). It binds
--- a render step at `Enum.RenderPriority.Camera` and follows its subject where
--- it is DRAWN -- `GetRenderCFrame` -- so it turns with the mouse in the frame
--- and the world does not shake behind it. Following the SIMULATED place every
--- frame is what shook: the subject moves once a tick and the camera every
--- frame, and the difference reads as the whole world vibrating (D047). That is
--- why the rig lived on `Heartbeat` until it could read where things are drawn.
---
--- **Call `rig:Update(dt)` on `Heartbeat` anyway.** Where a frame drives the
--- rig it does nothing; where none does -- a headless run, a gate, a demo's
--- autopilot -- it moves the camera once a tick, exactly as before, so a test
--- that drives the game records the picture it always did.
---
--- Casing follows ADR 0034: this is a module, so `camera.thirdPerson` is
--- camelCase, and the rig it returns is an object, so `rig:Update()` is not.

local InputService = game:GetService("InputService")
local RunService = game:GetService("RunService")

local Camera = {}

-- How many rigs this VM has made: each binds its render step under its own name.
local Rigs = 0

export type Rig = {
    --- The camera this rig writes. Assigned to `workspace.CurrentCamera` by the
    --- constructor, because a rig nothing looks through is a rig nobody can see
    --- is working.
    Camera: Camera,
    --- What it follows. Reassignable: a game that swaps the player's body swaps
    --- this rather than building a second rig.
    Subject: BasePart,

    --- Where the rig is looking, in radians, and how high above the subject it
    --- sits. Written by `Turn`, readable because a game that wants to face the
    --- character the way the camera faces needs this number.
    Yaw: number,
    Height: number,
    Distance: number,
    --- The actions the rig reads every frame (ADR 0136), from an
    --- `InputContext` at `Rate = Render`: an axis pair to turn by, and the
    --- pointer's motion to look by, read while the pointer is locked.
    --- Settable after the rig is made.
    TurnAction: InputAction?,
    LookAction: InputAction?,

    --- Turns the rig. `delta` is an axis pair in the range [-1, 1] -- an
    --- `InputAction` of type `Direction2D` hands you exactly this -- and `dt`
    --- makes the rate frame-independent.
    Turn: (self: Rig, delta: Vector2, dt: number) -> (),
    --- Turns the rig by a DISPLACEMENT rather than at a rate, which is what a
    --- mouse gives: `Enum.KeyCode.MouseMovement` reports the pointer's motion
    --- this tick in pixels, and that motion is already the amount to turn by.
    ---
    --- **Never scale a mouse by `dt`.** A stick is a rate -- held at full
    --- deflection it should turn the same amount per second whatever the frame
    --- rate -- and a mouse is a distance the hand already moved. Multiplying one
    --- by the other makes a mouse that turns twice as far on a slow machine,
    --- which is the single most common way a look control feels wrong.
    Look: (self: Rig, delta: Vector2) -> (),
    --- Moves the camera towards where it should be when no frame is drawn:
    --- call once per SIMULATION tick. Where a frame is drawn the rig's render
    --- step moves it instead, and this does nothing (see the note at the top).
    Update: (self: Rig, dt: number) -> (),
    --- The rig's own forward and right on the ground plane, which is what a
    --- character's movement should be expressed in: pressing "forward" means
    --- away from the camera, not along the world's -Z.
    Basis: (self: Rig) -> (vector, vector),
}

export type Options = {
    Subject: BasePart,
    --- Metres behind the subject. 11 is a third-person distance for a character
    --- about two metres tall.
    Distance: number?,
    --- Metres above it. The pair (distance, height) is the whole framing.
    Height: number?,
    --- Metres above the subject's origin that the camera aims at. Aiming at the
    --- feet puts the character at the top of the frame, which is the most common
    --- way a first third-person camera looks wrong.
    Focus: number?,
    --- Radians per second at full stick deflection.
    TurnRate: number?,
    --- **Actions the rig reads itself, every frame** (ADR 0136): an axis pair
    --- to `Turn` by and a pointer displacement to `Look` by, the pointer only
    --- while it is locked. Read only when their `InputContext` has
    --- `Rate = Render` -- at the simulation's rate an action holds a tick's
    --- worth, and reading it every frame would turn it several times over.
    --- Without them, call `rig:Turn` and `rig:Look`.
    TurnAction: InputAction?,
    LookAction: InputAction?,
    --- Radians per PIXEL of pointer motion, for `Look`. About a quarter of a
    --- degree per pixel, which is where most first-person defaults sit.
    LookSensitivity: number?,
    --- How fast the camera closes the distance to where it should be, per
    --- second. Higher is stiffer; the clamp inside `Update` is what stops a
    --- frame-rate hitch teleporting it.
    Smoothing: number?,
    MinHeight: number?,
    MaxHeight: number?,
    FieldOfView: number?,
    NearPlane: number?,
    FarPlane: number?,
}

local RigImpl = {}
RigImpl.__index = RigImpl

-- **Yaw DECREASES to turn right, and the minus signs below are why.** The rig's
-- forward is `(-sin y, 0, -cos y)`: at a yaw of zero it looks down -Z, and
-- increasing the yaw rotates it towards -X, which is to the viewer's LEFT. So a
-- positive input -- the right arrow, the stick pushed right, the mouse moved
-- right -- has to subtract. Getting this backwards produces a camera that turns
-- away from the way you pushed, which is what the first version of this module
-- did and what a human noticed within a minute.
function RigImpl.Turn(self: any, delta: Vector2, dt: number)
    self.Yaw -= self.TurnRate * delta.X * dt
    self.Height = math.clamp(self.Height + self.TurnRate * 3 * delta.Y * dt, self.MinHeight, self.MaxHeight)
end

function RigImpl.Look(self: any, delta: Vector2)
    self.Yaw -= self.LookSensitivity * delta.X
    self.Height = math.clamp(self.Height - self.LookSensitivity * 12 * delta.Y, self.MinHeight, self.MaxHeight)
end

function RigImpl.Basis(self: any): (vector, vector)
    local forward = vector.create(-math.sin(self.Yaw), 0, -math.cos(self.Yaw))
    local right = vector.create(math.cos(self.Yaw), 0, -math.sin(self.Yaw))
    return forward, right
end

-- Moves the camera towards where it should be, following a subject at `at`.
-- A subject that jumped further than the rig ever eases -- a teleport, a
-- respawn -- takes the camera with it, so the two land together instead of the
-- camera flying across the world behind it.
local function place(self: any, at: vector, dt: number)
    local focus = at + vector.create(0, self.Focus, 0)
    local forward = vector.create(-math.sin(self.Yaw), 0, -math.cos(self.Yaw))
    local wanted = focus - forward * self.Distance + vector.create(0, self.Height, 0)

    local eye = self.Camera.CFrame.Position
    if vector.magnitude(wanted - eye) > self.Distance * 4 then
        eye = wanted
    else
        -- Frame-rate independent, and the clamp is what keeps a hitch from
        -- teleporting the camera: at 5 fps `Smoothing * dt` exceeds one, and
        -- an unclamped lerp past its endpoint overshoots.
        eye += (wanted - eye) * math.min(1, self.Smoothing * dt)
    end
    self.Camera.CFrame = CFrame.lookAt(eye, focus)
end

-- Whether an action is sampled every frame, and so can be read in one.
local function everyFrame(action: InputAction?): boolean
    if action == nil then
        return false
    end
    local context = action.Parent
    return context ~= nil and context:IsA("InputContext") and (context :: any).Rate == Enum.InputRate.Render
end

-- The rig's render step: every drawn frame, where the subject is drawn.
local function frame(self: any, dt: number)
    self.Framed = true
    if everyFrame(self.TurnAction) then
        self:Turn(self.TurnAction:GetState() :: Vector2, dt)
    end
    -- The pointer turns it only while locked: unlocked, its motion belongs to
    -- whatever the player is doing with the cursor.
    if everyFrame(self.LookAction) and InputService.PointerLocked then
        self:Look(self.LookAction:GetState() :: Vector2)
    end
    self.Yaw += self.Spin * dt
    place(self, self.Subject:GetRenderCFrame().Position, dt)
end

function RigImpl.Update(self: any, dt: number)
    -- A frame drives it where one is drawn.
    if self.Framed then
        return
    end
    self.Yaw += self.Spin * dt
    place(self, self.Subject.Position, dt)
end

--- A third-person follow rig: behind and above the subject, turned by an axis
--- pair, easing towards where it should be.
---
--- ```lua
--- local camera = require("@engine/camera")
--- local rig = camera.thirdPerson({ Subject = character })
---
--- RunService.Heartbeat:Connect(function(dt)
---     rig:Turn(TurnAction:GetState() :: Vector2, dt)
---     local forward, right = rig:Basis()
---     -- ... move the character in that basis ...
---     rig:Update(dt)
--- end)
--- ```
function Camera.thirdPerson(options: Options): Rig
    --- **A camera the scene already holds is ADOPTED, not replaced.**
    ---
    --- The arrangement ADR 0047 asks projects to move to is the one Unity and
    --- Unreal have: the camera is an object in the scene and the script is what
    --- drives it. A rig that always made its own would put a second camera in
    --- every scene-first project, and the second one wins `CurrentCamera` --
    --- which is a scene whose saved camera is quietly ignored.
    ---
    --- A project with no scene gets exactly what it always got.
    local existing = workspace.CurrentCamera
    local instance = if existing ~= nil and existing:IsA("Camera") then existing else Instance.new("Camera")
    if existing == nil then
        instance.Name = "FollowCamera"
        instance.FieldOfView = options.FieldOfView or 70
        instance.NearPlane = options.NearPlane or 0.1
        instance.FarPlane = options.FarPlane or 1200
        instance.Parent = workspace
        workspace.CurrentCamera = instance
    end

    local rig = setmetatable({
        Camera = instance,
        Subject = options.Subject,
        Yaw = 0,
        Distance = options.Distance or 11,
        Height = options.Height or 6,
        Focus = options.Focus or 2,
        TurnRate = options.TurnRate or 2.2,
        LookSensitivity = options.LookSensitivity or 0.004,
        Smoothing = options.Smoothing or 8,
        MinHeight = options.MinHeight or 3,
        MaxHeight = options.MaxHeight or 24,
        TurnAction = options.TurnAction,
        LookAction = options.LookAction,
        -- Radians a second it turns by itself: `orbit`'s.
        Spin = 0,
        -- A frame has driven it: from then on `Update` leaves it to them.
        Framed = false,
    }, RigImpl)

    -- Placed before the first update rather than eased in from the origin: a
    -- rig that starts at (0, 0, 0) spends its first second flying across the
    -- world, which is what every screenshot of frame one would show.
    local rigAny = rig :: any
    rigAny.Camera.CFrame = CFrame.lookAt(
        options.Subject.Position + vector.create(0, rigAny.Height, rigAny.Distance),
        options.Subject.Position + vector.create(0, rigAny.Focus, 0)
    )

    -- Every drawn frame, at the camera's place in the frame (ADR 0136). The
    -- binding goes with the script that made the rig, and its scene.
    Rigs += 1
    RunService:BindToRenderStep(`engine.camera.rig{Rigs}`, Enum.RenderPriority.Camera.Value, function(dt: number)
        frame(rigAny, dt)
    end)

    return (rig :: any) :: Rig
end

--- An orbit rig: the same framing, turning on its own at a fixed rate. What a
--- menu background, a spectator view or a screenshot harness wants.
function Camera.orbit(options: Options & { Speed: number? }): Rig
    local rig = Camera.thirdPerson(options)
    local rigAny = rig :: any
    rigAny.Spin = options.Speed or 0.35
    return rig
end

return Camera
