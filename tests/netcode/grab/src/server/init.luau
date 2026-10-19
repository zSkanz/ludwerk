--!strict
--[[
	The authority: a table and two blocks on it, and `xrgrab.serve` answering
	the hands that close on them. It says, once the first joiner has lifted
	block A and let it go, who held A, how high it saw A go, and who has it
	after the settle:

	    [grab] server A held by 2 rose 0.30 m, then the authority's
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")
local xrgrab = require("@engine/xrgrab")

if not NetworkService.Authority then
    return
end

local function part(name: string, size: vector, position: vector, anchored: boolean): Part
    local made = Instance.new("Part")
    made.Name = name
    made.Anchored = anchored
    made.Size = size
    made.Position = position
    made.Parent = Workspace
    return made
end

local TableTop = 0.8
part("Table", vector.create(2, TableTop, 1), vector.create(0, TableTop / 2, 0), true)
local a = part("A", vector.create(0.15, 0.15, 0.15), vector.create(-0.4, TableTop + 0.075, 0), false)
part("B", vector.create(0.15, 0.15, 0.15), vector.create(0.4, TableTop + 0.075, 0), false)

-- Characters stand where the server puts them; the test's hands are said to
-- be at the blocks, so how far an arm reaches is not what is asked here.
xrgrab.serve({ Settle = 1, ArmLength = math.huge })

local heldBy = 0
local highest = 0.0
local reported = false
local releasedAt = 0
local ticks = 0
RunService.PostSimulation:Connect(function()
    ticks += 1
    local owner = a:GetNetworkOwner()
    if owner then
        heldBy = owner.UserId
        highest = math.max(highest, a.Position.y - (TableTop + 0.075))
        releasedAt = 0
    elseif heldBy ~= 0 and releasedAt == 0 then
        releasedAt = ticks
    end
    -- A settle and a half after A went back to the authority.
    if not reported and releasedAt ~= 0 and ticks - releasedAt > 30 then
        reported = true
        print(
            `[grab] server A held by {heldBy} rose {string.format("%.2f", highest)} m, then `
                .. `{if a:GetNetworkOwner() == nil then "the authority's" else "still a player's"}`
        )
    end
end)
