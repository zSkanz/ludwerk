--!strict
--[[
	Who walks where, decided by the authority: a `Script` under
	`GlobalScriptService.Server`, from `src/server/`. Solo this machine is the
	authority; in a match it is the server.

	**Nothing here plays an animation.** There is no `LoadAnimation`, no `Play`
	and no `RemoteEvent` that says "slash now". The three heroes each carry an
	`AnimationPlayer` with the same graph (`content/anim/hero.animgraph.json`),
	and the graph is driven by what a character already is on every machine:

	- how fast its `CharacterBody` is going, which way in its own frame, and
	  whether it is on the ground -- so `Move` and `Jump` below are the whole
	  of walking, running, strafing, backing away, jumping and falling;
	- two attributes on the body: `Attack`, which the graph takes as a trigger
	  -- it fires when the value CHANGES, so a counter is the idiomatic thing
	  to write -- and `Aim`, a number that weighs a lean on top of whatever
	  else is playing.

	What a script does write is who moves and which way they face.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

-- The level and the three heroes are the scene
-- (`content/scenes/character.scene.json`, written by `tools/make_scene.py`).
local tall = workspace:WaitForChild("Tall") :: CharacterBody
local stocky = workspace:WaitForChild("Stocky") :: CharacterBody
local small = workspace:WaitForChild("Small") :: CharacterBody

local function ahead(yaw: number): vector
    return vector.create(-math.sin(yaw), 0, -math.cos(yaw))
end

--- Turns a body to face `yaw` about the vertical, where it stands. Which way
--- a `CharacterBody` faces is the game's to say: `Move` walks it and does not
--- turn it, which is what lets one walk sideways and backwards.
local function face(body: CharacterBody, yaw: number)
    local at = body.Position
    body.CFrame = CFrame.lookAt(at, at + ahead(yaw))
end

-- --- The player's hero ---------------------------------------------------------

-- Metres a second. Forwards is a run, or a walk with the walk key held; back
-- and sideways are slower, as the clips for them are.
local RunSpeed = tall.WalkSpeed
local WalkSpeed = 1.5
local BackSpeed = 1.2
local StrafeSpeed = 0.8
-- Radians a second.
local TurnSpeed = 2.4

-- One hero to play, so one player has it: the first to arrive. A game with a
-- hero for everyone would place one here for each.
local hero: Player? = nil
local heroYaw = 0
local attacks = 0
local attackHeld = false

local function seat(player: Player)
    if hero == nil then
        hero = player
        -- What makes it theirs: a client that joined predicts its own
        -- character, and every machine's `Player` points at its copy of it.
        player.Character = tall
    end
end

for _, player in NetworkService:GetPlayers() do
    seat(player)
end
NetworkService.PlayerAdded:Connect(seat)
NetworkService.PlayerRemoving:Connect(function(player: Player)
    if hero == player then
        hero = nil
    end
end)

local function walkHero(player: Player, dt: number)
    local turn = player:GetIntent("Turn")
    if typeof(turn) == "Vector2" then
        heroYaw -= turn.X * TurnSpeed * dt
    end
    face(tall, heroYaw)

    local move = player:GetIntent("Move")
    if typeof(move) == "Vector2" then
        local forward = ahead(heroYaw)
        local right = vector.create(-forward.z, 0, forward.x)
        local pace = if move.Y < 0 then BackSpeed elseif player:GetIntent("Walk") == true then WalkSpeed else RunSpeed
        -- `Move` takes a direction and a throttle: a vector as long as the
        -- part of `WalkSpeed` that is wanted.
        tall:Move((forward * (move.Y * pace) + right * (move.X * StrafeSpeed)) / tall.WalkSpeed)
    end
    if player:GetIntent("Jump") == true and tall.Grounded then
        tall:Jump()
    end

    -- **The whole of an attack, on every machine**: one attribute, set to
    -- something it was not. Once a press, not once a tick the key is down.
    local attack = player:GetIntent("Attack") == true
    if attack and not attackHeld then
        attacks += 1
        tall:SetAttribute("Attack", attacks)
    end
    attackHeld = attack
end

-- --- The one who walks the route -------------------------------------------------

-- Along the lane the stairs, the platform and the ramp are on, and back along
-- the floor in front of them: the corners of `tools/make_scene.py`'s level.
local Route = {
    vector.create(-7.5, 0, -4),
    vector.create(6.8, 0, -4),
    vector.create(6.8, 0, 0.5),
    vector.create(-7.5, 0, 0.5),
}
local leg = 1
local stockyYaw = 0

local function walkRoute(dt: number)
    local at = stocky.Position
    local to = vector.create(Route[leg].x - at.x, 0, Route[leg].z - at.z)
    if vector.magnitude(to) < 0.15 then
        leg = leg % #Route + 1
        return
    end
    local heading = vector.normalize(to)
    -- It turns into a corner rather than snapping round it: the short way
    -- towards where it is going, no faster than it can turn.
    local wanted = math.atan2(-heading.x, -heading.z)
    local off = (wanted - stockyYaw + math.pi) % (2 * math.pi) - math.pi
    stockyYaw += math.clamp(off, -TurnSpeed * dt, TurnSpeed * dt)
    face(stocky, stockyYaw)
    stocky:Move(heading)
end

-- --- The one who stands and slashes ------------------------------------------------

local SlashEvery = 3.5
local slashes = 0
local untilSlash = 2
local clock = 0

local function drill(dt: number)
    clock += dt
    untilSlash -= dt
    if untilSlash <= 0 then
        untilSlash = SlashEvery
        slashes += 1
        small:SetAttribute("Attack", slashes)
    end
    -- Nought to one and back, about every eight seconds: how far it leans
    -- back, laid on top of its standing and its slash.
    small:SetAttribute("Aim", 0.5 - 0.5 * math.cos(clock * 0.8))
end

-- --- Who the player's hero looks at --------------------------------------------------

-- Each hero has an `IKControl` on its head and a `Bone` between its eyes for
-- the others to look at. The two that are not played look at the player's,
-- which the scene already says; the player's looks at whichever of them is
-- nearer, which changes, so it is set here -- and travels with the character
-- like any other property. Turning the head is each machine's own picture.
local look = (tall:WaitForChild("Hero") :: Instance):WaitForChild("Look") :: IKControl
local eyes: { [CharacterBody]: Attachment } = {
    [stocky] = (stocky:WaitForChild("Hero") :: Instance):WaitForChild("Eyes") :: Attachment,
    [small] = (small:WaitForChild("Hero") :: Instance):WaitForChild("Eyes") :: Attachment,
}

local function watch()
    local nearer = if vector.magnitude(stocky.Position - tall.Position)
            < vector.magnitude(small.Position - tall.Position)
        then stocky
        else small
    if look.Target ~= eyes[nearer] then
        look.Target = eyes[nearer]
    end
end

RunService.Heartbeat:Connect(function(dt: number)
    if hero then
        walkHero(hero, dt)
    end
    walkRoute(dt)
    drill(dt)
    watch()
end)

print(
    "37-character: WASD moves, Q and E (or the arrow keys) turn, Shift walks, Space jumps, F or the left mouse button slashes."
)
