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
--- local hold = xrgrab.hold(part, handCFrame)  -- the grip closes
--- hold:Move(handCFrame)                         -- every frame it is held
--- hold:Release()                                -- the grip opens
--- ```
---
--- **How strongly**: `Strength` in the options, or the part's `GrabStrength`
--- attribute over it, is the force the hold pulls with as a multiple of the
--- part's weight: 4 by default, which lifts it briskly and pushes on what it
--- meets with three times its weight. In an online game the holder's
--- machine has to simulate the part for it to move at once in their hand:
--- the authority gives it to them with `SetNetworkOwner` when they reach
--- for it.
---
--- Casing follows ADR 0034: `xrgrab.hold` is a module function; what it
--- returns is an object, so `hold:Move(...)`.

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
}

local DefaultStrength = 4
local DefaultResponsiveness = 30

local function weightOf(part: BasePart): number
    local gravity = vector.magnitude(workspace.Gravity)
    return part.AssemblyMass * (if gravity > 0 then gravity else 9.81)
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

    part.Anchored = false

    local hold = {} :: Hold
    hold.Part = part
    hold.Held = true
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
    end
    return hold
end

return XrGrab
