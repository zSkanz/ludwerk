--!strict
--[[
	The authority: a floor, and one character it spawns from a stamp -- with
	its animation player and the graph it names, a cape, a control, a foot
	placement, a light, a sound and a trail, all as the stamp authors them --
	and walks, runs, jumps and makes attack once somebody has joined.

	Nothing here plays an animation. The character is moved, and an attribute
	is set; the graph on each machine does the rest (ADR 0197).
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = vector.create(400, 1, 400)
floor.Position = vector.create(0, -0.5, 0)
floor.Parent = Workspace

local hero = Instance.stamp("walker") :: CharacterBody
hero.Position = vector.create(0, 1.5, 0)
hero:SetAttribute("Phase", "wait")
hero.Parent = Workspace

-- What the authority's own graph did, kept where the joiner can read it and
-- say it beside its own: the same states from the same inputs.
local animator = hero:FindFirstChildOfClass("AnimationPlayer") :: AnimationPlayer
local did: { string } = {}
animator.StateChanged:Connect(function(layer: string, from: string, to: string)
    -- From the walk on: before it the character is settling onto the floor
    -- it was spawned over, and a joiner may or may not be there to see that.
    local phase = hero:GetAttribute("Phase")
    if phase == "wait" or phase == "stand" then
        return
    end
    table.insert(did, `{layer}:{from}>{to}`)
    hero:SetAttribute("Did", table.concat(did, " "))
end)

local Forward = vector.create(0, 0, -1)
local started = false
local elapsed = 0
local jumped = false
local attacked = false

NetworkService.PlayerAdded:Connect(function()
    started = true
end)
-- The joiner leaves when it has said what it saw; there is nobody else.
NetworkService.PlayerRemoving:Connect(function()
    game:Shutdown()
end)

local function phase(name: string)
    if hero:GetAttribute("Phase") ~= name then
        hero:SetAttribute("Phase", name)
    end
end

RunService.Heartbeat:Connect(function(dt: number)
    if not started then
        hero:Move(vector.zero)
        return
    end
    elapsed += dt
    if elapsed < 2 then
        phase("stand")
        hero:Move(vector.zero)
    elseif elapsed < 5 then
        phase("walk")
        hero.WalkSpeed = 3
        hero:Move(Forward)
    elseif elapsed < 8 then
        phase("run")
        hero.WalkSpeed = 8
        hero:Move(Forward)
    elseif elapsed < 11 then
        phase("jump")
        hero:Move(vector.zero)
        if not jumped and elapsed > 8.5 then
            jumped = true
            hero:Jump()
        end
    elseif elapsed < 13 then
        phase("attack")
        hero:Move(vector.zero)
        if not attacked and elapsed > 11.25 then
            attacked = true
            -- The whole of a server-side attack.
            hero:SetAttribute("Attack", 1)
        end
    else
        phase("done")
        hero:Move(vector.zero)
    end
end)
