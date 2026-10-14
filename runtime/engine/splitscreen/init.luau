--!strict
--- `@engine/splitscreen` -- one picture while the players are together, a
--- view each when they walk apart (ADR 0205).
---
--- For a game seen from above or from the side with two to four players at
--- one machine. The engine's part is `Workspace.ScreenSplit = Dynamic`: one
--- picture at `SplitBlend` 0, the players' views put together above it. This
--- is the rule that drives those numbers and the cameras, so the common case
--- is a line:
---
--- ```lua
--- local splitscreen = require("@engine/splitscreen")
--- local screen = splitscreen.follow({ SplitDistance = 24 })
--- screen:Add(player, character)   -- once a player
--- RunService.Heartbeat:Connect(function(dt)
---     screen:Update(dt)
--- end)
--- ```
---
--- **Two players: a line that turns.** While they are within `SplitDistance`
--- of each other one camera frames them both, pulling back as they part. At
--- that distance the picture is exactly what each player's own camera would
--- show -- each looks at the point half of `SplitDistance` from its player
--- towards the other, which is then the middle between them -- so the screen
--- parts with no cut: a line appears across the direction between the two,
--- each player holds the middle of their own side, and the line turns as
--- they walk round each other.
---
--- **Three and four: the grid, faded in.** A line through the middle of a
--- window makes two regions and no more, so the turning line is the
--- two-player form. With more, one camera frames everybody while they are
--- together and each has a quarter of the window when they spread, and the
--- one fades into the other over `FadeTime` where a cut would be seen.
---
--- **It moves the cameras every drawn frame, by itself**, as the rigs of
--- `@engine/camera` do and for their reason (ADR 0136): it follows each
--- subject where it is DRAWN. Call `screen:Update(dt)` on `Heartbeat` anyway;
--- where a frame drives it that does nothing, and where none does -- a
--- headless run, a gate -- it moves the cameras once a tick.
---
--- It is Luau, in the open. A game that wants another rule -- a side view, a
--- line that never turns, three views of unequal size -- copies it and writes
--- `Workspace.SplitBlend` and `SplitAngle` itself.
---
--- Casing follows ADR 0034: `splitscreen.follow` is a module function; what
--- it returns is an object, so `screen:Add()`.

local RunService = game:GetService("RunService")

local SplitScreen = {}

-- How many screens this VM has made: each binds its render step by its own name.
local Screens = 0

export type Options = {
    --- From the point a camera looks at to the camera, in metres: above and
    --- behind for a game seen from above. `vector.create(0, 26, 16)` by default.
    Offset: vector?,
    --- How far apart two players may be before the screen parts, in metres
    --- (for three and four, how far the farthest pair may be). 22 by default.
    --- **Choose it so the one camera, at `Offset`, holds two players that far
    --- apart whichever way they stand**: that is the picture the screen parts
    --- from.
    SplitDistance: number?,
    --- How close the one camera comes while everybody stands together, as a
    --- fraction of `Offset`. 0.6 by default; 1 never zooms.
    CloseScale: number?,
    --- The vertical field of view of every camera it makes, in degrees. 50
    --- by default.
    FieldOfView: number?,
    --- How long the grid of three and four takes to fade in and out, in
    --- seconds. 0.35 by default.
    FadeTime: number?,
}

export type Screen = {
    --- The one camera that frames everybody. `workspace.CurrentCamera`,
    --- adopted when the scene has one and made when it has none.
    Camera: Camera,
    --- Yours to change while it runs.
    Offset: vector,
    SplitDistance: number,
    CloseScale: number,
    FadeTime: number,
    --- Gives `player` a view that follows `subject`. Returns the camera made
    --- for it, which is the player's `Camera`. Players are placed in the order
    --- they are added; four at most have a view.
    Add: (self: Screen, player: Player, subject: BasePart) -> Camera,
    --- Takes a player's view away, and its camera.
    Remove: (self: Screen, player: Player) -> (),
    --- Call on `Heartbeat`: moves the cameras where no drawn frame does.
    Update: (self: Screen, dt: number) -> (),
    --- Stops, puts the screen back to one picture, and removes the cameras
    --- it made.
    Destroy: (self: Screen) -> (),
}

type Seat = {
    Player: Player,
    Subject: BasePart,
    Camera: Camera,
}

local ScreenImpl = {}
ScreenImpl.__index = ScreenImpl

-- How far past `SplitDistance`, as a fraction of it, the line takes to reach
-- its full width: it appears and goes rather than cutting in.
local LineSoftness = 0.2
-- Three and four come back together a little inside the distance they parted
-- at, so a group standing at the edge of it does not fade in and out.
local Together = 0.85

local function look(camera: Camera, focus: vector, offset: vector)
    camera.CFrame = CFrame.lookAt(focus + offset, focus)
end

-- Places every camera for the subjects' places `at`, in seat order.
local function place(self: any, at: { vector }, dt: number)
    local seats: { Seat } = self.Seats
    local count = #at
    if count == 0 then
        workspace.SplitBlend = 0
        return
    end

    local centre = vector.zero
    for _, position in at do
        centre += position
    end
    centre /= count
    -- The farthest pair, which is what "apart" means for any number.
    local apart = 0
    for first = 1, count do
        for second = first + 1, count do
            apart = math.max(apart, vector.magnitude(at[first] - at[second]))
        end
    end

    local distance: number = self.SplitDistance
    local close: number = self.CloseScale
    local offset: vector = self.Offset
    -- Closer while they stand together, at the whole offset by the time the
    -- screen parts, and no farther after it.
    local reach = math.clamp(apart / distance, 0, 1)
    local shared = offset * (close + (1 - close) * reach)
    look(self.Camera, centre, shared)

    if count == 1 then
        workspace.SplitBlend = 0
        look(seats[1].Camera, at[1], shared)
        return
    end

    if count == 2 then
        -- Each looks half of `SplitDistance` from its own player towards the
        -- other, and no farther than the middle between them: together that
        -- IS the middle, and the two pictures are the one picture.
        local between = at[2] - at[1]
        local towards = if apart > 1e-4 then between / apart else vector.zero
        local lead = math.min(apart, distance) / 2
        look(seats[1].Camera, at[1] + towards * lead, shared)
        look(seats[2].Camera, at[2] - towards * lead, shared)

        local blend = math.clamp((apart - distance) / (distance * LineSoftness), 0, 1)
        workspace.SplitBlend = blend
        if blend > 0 then
            -- The way from the first to the second on the screen: the line
            -- runs across it.
            local frame = self.Camera.CFrame
            local right = vector.dot(between, frame.RightVector)
            local up = vector.dot(between, frame.UpVector)
            workspace.SplitAngle = math.deg(math.atan2(up, right))
        end
        return
    end

    -- Three and four: a view each, on its own player, and the grid faded in.
    for index, seat in seats do
        if at[index] ~= nil then
            look(seat.Camera, at[index], offset)
        end
    end
    if apart > distance then
        self.Parted = true
    elseif apart < distance * Together then
        self.Parted = false
    end
    local fade: number = self.FadeTime
    local step = if fade > 0 then dt / fade else 1
    local blend: number = workspace.SplitBlend
    workspace.SplitBlend = if self.Parted then math.min(blend + step, 1) else math.max(blend - step, 0)
end

-- The seats whose subject is still in the world, and where each is: drawn
-- (`drawn`) or simulated.
local function places(self: any, drawn: boolean): { vector }
    local seats: { Seat } = self.Seats
    local at: { vector } = {}
    for index = #seats, 1, -1 do
        if not seats[index].Subject:IsDescendantOf(game) or not seats[index].Player:IsDescendantOf(game) then
            self:Remove(seats[index].Player)
        end
    end
    for _, seat in seats do
        table.insert(at, if drawn then seat.Subject:GetRenderCFrame().Position else seat.Subject.Position)
    end
    return at
end

function ScreenImpl.Add(self: any, player: Player, subject: BasePart): Camera
    self:Remove(player)
    local camera = Instance.new("Camera")
    camera.Name = `SplitCamera{player.UserId}`
    camera.FieldOfView = self.Camera.FieldOfView
    camera.NearPlane = self.Camera.NearPlane
    camera.FarPlane = self.Camera.FarPlane
    camera.Parent = workspace
    player.Camera = camera
    table.insert(self.Seats, { Player = player, Subject = subject, Camera = camera })
    -- Placed now, and not eased in from the origin.
    place(self, places(self, false), 0)
    return camera
end

function ScreenImpl.Remove(self: any, player: Player)
    local seats: { Seat } = self.Seats
    for index, seat in seats do
        if seat.Player == player then
            table.remove(seats, index)
            if player:IsDescendantOf(game) and player.Camera == seat.Camera then
                player.Camera = nil
            end
            seat.Camera:Destroy()
            return
        end
    end
end

function ScreenImpl.Update(self: any, dt: number)
    -- A frame drives it where one is drawn.
    if self.Framed or self.Step == nil then
        return
    end
    place(self, places(self, false), dt)
end

function ScreenImpl.Destroy(self: any)
    if self.Step == nil then
        return
    end
    RunService:UnbindFromRenderStep(self.Step)
    self.Step = nil
    local seats: { Seat } = self.Seats
    for index = #seats, 1, -1 do
        self:Remove(seats[index].Player)
    end
    workspace.SplitBlend = 0
    workspace.ScreenSplit = Enum.ScreenSplit.None
    if self.Made then
        self.Camera:Destroy()
    end
end

--- Starts a dynamic split: sets `Workspace.ScreenSplit` to `Dynamic` and
--- frames whoever is added.
function SplitScreen.follow(options: Options?): Screen
    local given: Options = options or {}
    -- A camera the scene already holds is adopted, as a rig adopts one.
    local existing = workspace.CurrentCamera
    local made = existing == nil or not existing:IsA("Camera")
    local camera = if made then Instance.new("Camera") else existing :: Camera
    if made then
        camera.Name = "SharedCamera"
        camera.FieldOfView = given.FieldOfView or 50
        camera.NearPlane = 0.1
        camera.FarPlane = 1200
        camera.Parent = workspace
        workspace.CurrentCamera = camera
    elseif given.FieldOfView ~= nil then
        camera.FieldOfView = given.FieldOfView
    end

    local screen = setmetatable({
        Camera = camera,
        Offset = given.Offset or vector.create(0, 26, 16),
        SplitDistance = given.SplitDistance or 22,
        CloseScale = given.CloseScale or 0.6,
        FadeTime = given.FadeTime or 0.35,
        Seats = {} :: { Seat },
        Made = made,
        Parted = false,
        -- A frame has driven it: from then on `Update` leaves it to them.
        Framed = false,
    }, ScreenImpl)
    local screenAny = screen :: any

    workspace.SplitBlend = 0
    workspace.ScreenSplit = Enum.ScreenSplit.Dynamic

    Screens += 1
    screenAny.Step = `engine.splitscreen{Screens}`
    RunService:BindToRenderStep(screenAny.Step, Enum.RenderPriority.Camera.Value, function(dt: number)
        screenAny.Framed = true
        place(screenAny, places(screenAny, true), dt)
    end)

    return (screen :: any) :: Screen
end

return SplitScreen
