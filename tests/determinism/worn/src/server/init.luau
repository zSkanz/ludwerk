--!strict
--- A piece that wears a body's pose (ADR 0201, `MeshPart.PoseFrom`), over ten
--- seconds: put on, taken off and put on again while the body walks, runs
--- and is made to attack.
---
--- **A follower's pose is simulation exactly as far as its leader's is.** A
--- `Bone` on a joint of the piece is read by the tick -- a muzzle on a slide --
--- so where it is has to reproduce. Each tick a marker part is put where that
--- bone is, which is how the piece's pose reaches the world's hash: a build
--- that made a follower's pose from the frame's picture, or in an order that
--- changed, moves the trace.
---
--- The content is the graph scenario's (`tests/determinism/graph`), copied.
--- The assertions are `error` calls, which fail a replay.

local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(400, 1, 400)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = Workspace

local hero = Instance.new("CharacterBody")
hero.Name = "Hero"
hero.Size = vector.create(1, 2, 1)
hero.Position = vector.create(0, 1.5, 0)

local body = Instance.new("MeshPart")
body.Name = "Body"
body.MeshContent = "asset://models/walker.gltf"
body.Anchored = true
body.CanCollide = false
body.Parent = hero

local hand = Instance.new("Bone")
hand.Name = "Hand"
hand.JointName = "Arm"
hand.Parent = body

-- The piece: the same skeleton, left a long way from the body. Where its own
-- part is must not matter while it is worn.
local piece = Instance.new("MeshPart")
piece.Name = "Piece"
piece.MeshContent = "asset://models/walker.gltf"
piece.Anchored = true
piece.CanCollide = false
piece.Position = vector.create(50, 5, 50)
piece.PoseFrom = body
piece.Parent = hero

local muzzle = Instance.new("Bone")
muzzle.Name = "Muzzle"
muzzle.JointName = "Arm"
muzzle.Parent = piece

local animator = Instance.new("AnimationPlayer")
animator.Graph = "asset://anim/walker.animgraph.json"
animator.Parent = hero
hero.Parent = Workspace

local marker = Instance.new("Part")
marker.Name = "Marker"
marker.Anchored = true
marker.CanCollide = false
marker.Size = vector.create(0.1, 0.1, 0.1)
marker.Parent = Workspace

local Forward = vector.create(0, 0, -1)
local elapsed = 0
local ticks = 0
local attacked = false
local together = 0
local apart = 0

RunService.Heartbeat:Connect(function(dt: number)
    elapsed += dt
    ticks += 1
    if elapsed < 1 then
        hero:Move(vector.zero)
    elseif elapsed < 5 then
        hero.WalkSpeed = 3
        hero:Move(Forward)
    else
        hero.WalkSpeed = 8
        hero:Move(Forward)
        if not attacked and elapsed > 6 then
            attacked = true
            hero:SetAttribute("Attack", 1)
        end
    end

    -- Taken off for a second in the middle, and put on again.
    if ticks == 240 then
        piece.PoseFrom = nil
    elseif ticks == 300 then
        piece.PoseFrom = body
    end

    -- Where the piece's bone is, written into the world.
    marker.Position = muzzle.WorldCFrame.Position

    -- Worn, the piece's joint is the body's: the two bones are one place,
    -- though the piece's own part is fifty metres away. Off, its bone is on
    -- its own part again.
    local between = vector.magnitude(muzzle.WorldCFrame.Position - hand.WorldCFrame.Position)
    if ticks > 4 and (ticks < 238 or ticks > 304) then
        if between > 0.001 then
            error(`worn: the piece's bone is {between} m from the body's at tick {ticks}`)
        end
        together += 1
    elseif ticks > 244 and ticks < 298 then
        if between < 10 then
            error(`worn: taken off, the piece's bone is still {between} m from the body's at tick {ticks}`)
        end
        apart += 1
    end
    if ticks == 590 and (together < 500 or apart < 50) then
        error(`worn: {together} ticks together and {apart} apart were judged`)
    end
end)
