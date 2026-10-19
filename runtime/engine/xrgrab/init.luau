--!strict
--- `@engine/xrgrab` -- holding a thing in a headset as a hand would.
---
--- A held thing moved by setting its `CFrame` is a body of no mass and no
--- limit: set on top of another and pushed down, it drives the one under it
--- into the table, which the solver cannot push back out of a static one.
--- This module holds it the way physics grabs are usually made: an
--- `AlignPosition` and an `AlignOrientation` pull it to the hand with a force
--- of their own, a few times its weight. What it meets stops it, and the hand
--- goes on into the table without it. Let go, it keeps the velocity it was
--- moving at, which is the hand's: a throw.
---
--- ```luau
--- local xrgrab = require("@engine/xrgrab")
--- xrgrab.serve()                                -- once, by the authority
--- local hold = xrgrab.hold(part, handCFrame)  -- the grip closes
--- hold:Move(handCFrame)                         -- every tick it is held
--- hold:Release()                                -- the grip opens
--- ```
---
--- **How strongly**: `Strength` in the options, or the part's `GrabStrength`
--- attribute over it, is the force the hold pulls with as a multiple of the
--- part's weight: 4 by default, which lifts it briskly and pushes on what it
--- meets with three times its weight.
---
--- **In an online game** the holder's machine has to simulate the part for
--- it to move at once in their hand. `serve`, run by the authority, answers
--- a hand that closes on a part: the part is the holder's
--- (`SetNetworkOwner`) where it is loose, within reach of the hand, held by
--- nobody else, and the game's own rule allows. The hand holds it at once
--- and lets go if the answer is no. Let go, the part stays the thrower's for
--- `Settle` seconds, so its flight is theirs, and then returns to the
--- authority. Without `serve`, a hold is the machine's own, as in a game
--- for one.
---
--- Casing follows ADR 0034: `xrgrab.hold` is a module function; what it
--- returns is an object, so `hold:Move(...)`.

local NetworkService = game:GetService("NetworkService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local XrGrab = {}

export type Options = {
    --- The pull, as a multiple of the part's weight. 4 by default; the
    --- part's `GrabStrength` attribute is used over it.
    Strength: number?,
    --- How quickly the part closes on the hand, in radians a second. 30 by
    --- default: a tenth of a second behind a quick hand, and no shudder at
    --- the simulation's sixty ticks under a display's seventy-two or ninety.
    Responsiveness: number?,
}

export type Hold = {
    --- The part held.
    Part: BasePart,
    --- Where the hand is now: the part is pulled to it.
    Move: (self: Hold, hand: CFrame) -> (),
    --- Lets go: the movers go, and the part keeps the velocity it has.
    Release: (self: Hold) -> (),
    --- Whether it is still held.
    Held: boolean,
    --- Whether the authority said no, which let it go.
    Refused: boolean,
}

export type ServeOptions = {
    --- The game's own word on whether `player` may hold `part`, after the
    --- module's. Every loose part within reach by default.
    Allow: ((player: Player, part: BasePart) -> boolean)?,
    --- Whether a part another player holds may be taken from them. False by
    --- default: the second hand is refused.
    Take: boolean?,
    --- Seconds a part let go stays with its thrower before it returns to
    --- the authority. 2 by default; `math.huge` keeps it theirs.
    Settle: number?,
    --- How far from the part a hand may say it is, in meters. 0.5 by default.
    Reach: number?,
    --- How far from the player's character, where they have one, a hand may
    --- say it is, in meters. 2 by default.
    ArmLength: number?,
}

local RemoteName = "XRGrab"
local DefaultStrength = 4
local DefaultResponsiveness = 30

-- This machine's holds, by part: the authority's answer finds them.
local Holding: { [BasePart]: Hold } = {}
local Listening = false

local function remote(): RemoteEvent?
    local found = ReplicatedStorage:FindFirstChild(RemoteName)
    return if found and found:IsA("RemoteEvent") then found :: RemoteEvent else nil
end

local function weightOf(part: BasePart): number
    local gravity = vector.magnitude(workspace.Gravity)
    return part.AssemblyMass * (if gravity > 0 then gravity else 9.81)
end

-- The authority's answers, heard once a machine first holds something.
local function listen(channel: RemoteEvent)
    if Listening then
        return
    end
    Listening = true
    channel.ClientReceived:Connect(function(answer: any, part: any)
        if answer ~= "refused" or typeof(part) ~= "Instance" then
            return
        end
        local hold = Holding[part :: BasePart]
        if hold then
            hold.Refused = true
            hold:Release()
        end
    end)
end

--- Holds `part` in a hand at `hand`, where it is now: the grip's place on
--- it is kept as the hand moves.
function XrGrab.hold(part: BasePart, hand: CFrame, options: Options?): Hold
    local strength = DefaultStrength
    local responsiveness = DefaultResponsiveness
    if options then
        strength = options.Strength or strength
        responsiveness = options.Responsiveness or responsiveness
    end
    local own = part:GetAttribute("GrabStrength")
    if typeof(own) == "number" then
        strength = own
    end

    -- The grip: where the hand closed, turned as the hand was.
    local grip = Instance.new("Attachment")
    grip.Name = "XRGrabGrip"
    grip.CFrame = part.CFrame:Inverse() * hand
    grip.Parent = part

    local force = weightOf(part) * strength
    local pull = Instance.new("AlignPosition")
    pull.Name = "XRGrabPull"
    pull.Mode = Enum.PositionAlignmentMode.OneAttachment
    pull.Attachment0 = grip
    pull.Position = hand.Position
    pull.MaxForce = force
    pull.Responsiveness = responsiveness
    pull.RigidityEnabled = false
    pull.Parent = part

    -- Torque enough to turn it as briskly, about its own size.
    local size = part.Size
    local reach = math.max(size.x, size.y, size.z)
    local turn = Instance.new("AlignOrientation")
    turn.Name = "XRGrabTurn"
    turn.Mode = Enum.OrientationAlignmentMode.OneAttachment
    turn.Attachment0 = grip
    turn.CFrame = hand
    turn.MaxTorque = force * reach
    turn.Responsiveness = responsiveness
    turn.RigidityEnabled = false
    turn.Parent = part

    local channel = remote()
    -- A part is loosed by the authority, which a client's copy cannot do.
    if NetworkService.Authority then
        part.Anchored = false
    end

    local hold = {} :: Hold
    hold.Part = part
    hold.Held = true
    hold.Refused = false
    function hold.Move(self: Hold, at: CFrame)
        if not self.Held then
            return
        end
        pull.Position = at.Position
        turn.CFrame = at
    end
    function hold.Release(self: Hold)
        if not self.Held then
            return
        end
        self.Held = false
        pull:Destroy()
        turn:Destroy()
        grip:Destroy()
        if Holding[part] == self then
            Holding[part] = nil
        end
        if channel and not self.Refused then
            channel:FireServer("release", part)
        end
    end
    -- Asked, and held at once: the answer lets it go if it is no.
    if channel then
        Holding[part] = hold
        listen(channel)
        channel:FireServer("grab", part, hand.Position)
    end
    return hold
end

--- **Answers the hands that close on parts**, on the authority: makes the
--- channel they ask through, and gives a part to the player who holds it.
function XrGrab.serve(options: ServeOptions?)
    assert(NetworkService.Authority, "xrgrab.serve runs on the authority")
    local opts: ServeOptions = options or {}
    local settle = opts.Settle or 2
    local reach = opts.Reach or 0.5
    local armLength = opts.ArmLength or 2
    local channel = remote()
    if channel == nil then
        local made = Instance.new("RemoteEvent")
        made.Name = RemoteName
        made.Parent = ReplicatedStorage
        channel = made
    end
    local answers = channel :: RemoteEvent
    -- Who holds each part now, and when each was let go.
    local holders: { [BasePart]: Player } = {}
    local letGo: { [BasePart]: number } = {}
    local releases = 0

    local function owned(part: BasePart, player: Player?)
        -- The authority's own player is the authority: nothing to give.
        pcall(function()
            part:SetNetworkOwner(player)
        end)
    end

    answers.ServerReceived:Connect(function(player: Player, what: any, thing: any, at: any)
        if typeof(thing) ~= "Instance" or not (thing :: Instance):IsA("BasePart") then
            return
        end
        local part = thing :: BasePart
        if what == "grab" then
            local holder = holders[part]
            local within = typeof(at) == "vector" and vector.magnitude(part.Position - (at :: vector)) <= reach
            local character = player.Character
            if within and character and typeof(at) == "vector" then
                within = vector.magnitude(character.Position - (at :: vector)) <= armLength
            end
            local allowed = part:IsDescendantOf(workspace)
                and not part.Anchored
                and within
                and (holder == nil or holder == player or opts.Take == true)
                and (opts.Allow == nil or (opts.Allow :: (Player, BasePart) -> boolean)(player, part))
            if not allowed then
                answers:FireClient(player, "refused", part)
                return
            end
            holders[part] = player
            letGo[part] = nil
            owned(part, player)
            answers:FireClient(player, "granted", part)
        elseif what == "release" and holders[part] == player then
            holders[part] = nil
            releases += 1
            local stamp = releases
            letGo[part] = stamp
            if settle ~= math.huge then
                task.delay(settle, function()
                    if letGo[part] == stamp and holders[part] == nil and part.Parent then
                        letGo[part] = nil
                        owned(part, nil)
                    end
                end)
            end
        end
    end)
end

return XrGrab
