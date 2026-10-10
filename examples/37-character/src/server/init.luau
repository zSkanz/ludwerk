--!strict
--[[
	Who walks where, decided by the authority: a `Script` under
	`GlobalScriptService.Server`, from `src/server/`. Solo this machine is the
	authority; in a match it is the server.

	**Nothing here plays an animation.** There is no `LoadAnimation`, no `Play`
	and no `RemoteEvent` that says "shoot now". The three heroes each carry an
	`AnimationPlayer` with the same graph (`content/anim/hero.animgraph.json`),
	and the graph is driven by what a character already is on every machine:

	- how fast its `CharacterBody` is going, which way in its own frame, and
	  whether it is on the ground -- so `Move` and `Jump` below are the whole
	  of walking, running, strafing, backing away, jumping and falling;
	- three attributes on the body: `Shoot` and `Attack`, which the graph
	  takes as triggers -- one fires when the value CHANGES, so a counter is
	  the idiomatic thing to write -- and `Aim`, a number that weighs a lean
	  on top of whatever else is playing.

	What a script does write is who moves and which way they face -- and what
	the player's hero wears and holds, which is two properties: a piece's
	`PoseFrom`, and whose child a held thing is.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")

-- The level and the three heroes are the scene
-- (`content/scenes/character.scene.json`, written by `tools/make_scene.py`).
local rogue = workspace:WaitForChild("Rogue") :: CharacterBody
local mannequin = workspace:WaitForChild("Mannequin") :: CharacterBody
local skeleton = workspace:WaitForChild("Skeleton") :: CharacterBody

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

-- Metres a second, and each is what a clip of the library covers on this
-- body: its run, its walk, its walk backwards. The library's sidestep is a
-- run to the side and somewhat ahead, so that is where A and D go.
local RunSpeed = rogue.WalkSpeed
local WalkSpeed = 0.54
local BackSpeed = 0.55
local StrafeSide = 2.2
local StrafeAhead = 1.27
-- Radians a second.
local TurnSpeed = 2.4

-- One hero to play, so one player has it: the first to arrive. A game with a
-- hero for everyone would place one here for each.
local hero: Player? = nil
local heroYaw = 0
local shots = 0

-- What was down a tick ago: a press is acted on once, not once a tick.
local held: { [string]: boolean } = {}

local function pressed(player: Player, intent: string): boolean
    local down = player:GetIntent(intent) == true
    local was = held[intent] == true
    held[intent] = down
    return down and not was
end

local function seat(player: Player)
    if hero == nil then
        hero = player
        -- What makes it theirs: a client that joined predicts its own
        -- character, and every machine's `Player` points at its copy of it.
        player.Character = rogue
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

-- --- What the player's hero wears and holds ------------------------------------------

local body = rogue:WaitForChild("Hero") :: MeshPart
local rightHold = body:WaitForChild("RightHold") :: Attachment
local leftHold = body:WaitForChild("LeftHold") :: Attachment
local crossbow = rightHold:WaitForChild("Crossbow") :: MeshPart
local head = rogue:WaitForChild("Head") :: MeshPart
local Heads: { Content } = { "asset://models/rogue_hood.glb" :: Content, "asset://models/rogue_head.glb" :: Content }
local wearing = 1

--- **From one hand to the other**: the crossbow hangs from whichever of the
--- body's bones it is a child of. Nothing else is said -- no weld, no
--- `CFrame` -- and it is in the other hand on the next tick, on every
--- machine, with its string where the clip being played has it.
local function handOver()
    crossbow.Parent = if crossbow.Parent == rightHold then leftHold else rightHold
end

--- **One head for another**: the old piece goes and a new one is made, named
--- the body in `PoseFrom` and parented. The frame after, it is on the body,
--- joint for joint -- turned by the look, lowered by the feet on a stair.
local function changeHead()
    wearing = wearing % #Heads + 1
    local piece = Instance.new("MeshPart")
    piece.Name = "Head"
    piece.MeshContent = Heads[wearing]
    piece.CanCollide = false
    piece.PoseFrom = body
    piece.Parent = rogue
    head:Destroy()
    head = piece
end

local function walkHero(player: Player, dt: number)
    local turn = player:GetIntent("Turn")
    if typeof(turn) == "Vector2" then
        heroYaw -= turn.X * TurnSpeed * dt
    end
    face(rogue, heroYaw)

    local move = player:GetIntent("Move")
    if typeof(move) == "Vector2" then
        local forward = ahead(heroYaw)
        local right = vector.create(-forward.z, 0, forward.x)
        local pace = if move.Y < 0 then BackSpeed elseif player:GetIntent("Walk") == true then WalkSpeed else RunSpeed
        local along = move.Y * pace
        if move.Y == 0 then
            along = math.abs(move.X) * StrafeAhead
        end
        -- `Move` takes a direction and a throttle: a vector as long as the
        -- part of `WalkSpeed` that is wanted.
        rogue:Move((forward * along + right * (move.X * StrafeSide)) / rogue.WalkSpeed)
    end
    if player:GetIntent("Jump") == true and rogue.Grounded then
        rogue:Jump()
    end

    -- **The whole of a shot, on every machine**: one attribute, set to
    -- something it was not. The graph plays the shot and then the reload on
    -- the arms; the crossbow's string is a joint those two clips move.
    if pressed(player, "Shoot") then
        shots += 1
        rogue:SetAttribute("Shoot", shots)
    end
    if pressed(player, "Hand") then
        handOver()
    end
    if pressed(player, "Hood") then
        changeHead()
    end
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
local walkerYaw = 0

local function walkRoute(dt: number)
    local at = skeleton.Position
    local to = vector.create(Route[leg].x - at.x, 0, Route[leg].z - at.z)
    if vector.magnitude(to) < 0.15 then
        leg = leg % #Route + 1
        return
    end
    local heading = vector.normalize(to)
    -- It turns into a corner rather than snapping round it: the short way
    -- towards where it is going, no faster than it can turn.
    local wanted = math.atan2(-heading.x, -heading.z)
    local off = (wanted - walkerYaw + math.pi) % (2 * math.pi) - math.pi
    walkerYaw += math.clamp(off, -TurnSpeed * dt, TurnSpeed * dt)
    face(skeleton, walkerYaw)
    skeleton:Move(heading)
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
        mannequin:SetAttribute("Attack", slashes)
    end
    -- Nought to one and back, about every eight seconds: how far it leans
    -- back, laid on top of its standing and its slash.
    mannequin:SetAttribute("Aim", 0.5 - 0.5 * math.cos(clock * 0.8))
end

-- --- Who the player's hero looks at --------------------------------------------------

-- Each hero has an `IKControl` on its head and a `Bone` between its eyes for
-- the others to look at. The two that are not played look at the player's,
-- which the scene already says; the player's looks at whichever of them is
-- nearer, which changes, so it is set here -- and travels with the character
-- like any other property. Turning the head is each machine's own picture,
-- and the head that is worn turns with it.
local look = body:WaitForChild("Look") :: IKControl
local eyes: { [CharacterBody]: Attachment } = {
    [skeleton] = (skeleton:WaitForChild("Hero") :: Instance):WaitForChild("Eyes") :: Attachment,
    [mannequin] = (mannequin:WaitForChild("Hero") :: Instance):WaitForChild("Eyes") :: Attachment,
}

local function watch()
    local nearer = if vector.magnitude(skeleton.Position - rogue.Position)
            < vector.magnitude(mannequin.Position - rogue.Position)
        then skeleton
        else mannequin
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
    "37-character: WASD moves, Q and E (or the arrow keys) turn, Shift walks, Space jumps, "
        .. "F or the left mouse button shoots, G changes hands, H changes the head."
)
