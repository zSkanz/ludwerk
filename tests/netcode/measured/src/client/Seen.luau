--!strict
--[[
	The joiner: each of the server's parts as it arrived, against what the
	authority holds, then one of its own. One line a part:

	    [measured] Served same MeshSize=... Size=... Inside=Served Outside=miss
	    [measured] Mine MeshSize=... Size=...
	    [measured] Bare MeshSize=... Size=...

	`Bare` is the server's part with no mesh, named here: a copy of the
	server's is never measured here, so it keeps the server's numbers.
]]

local NetworkService = game:GetService("NetworkService")
local Workspace = game:GetService("Workspace")

local Measured = require("@shared/Measured")

if NetworkService.Authority then
    return
end

local held = Workspace:WaitForChild("Held", 20) :: RemoteFunction
for _, name in { "Served", "Sized", "Missing", "Bare" } do
    Workspace:WaitForChild(name, 20)
end
local theirs = held:InvokeServerAsync() :: { [string]: string }

-- A snapshot is a few ticks behind the answer to a remote.
local deadline = os.clock() + 15
while os.clock() < deadline do
    local all = true
    for _, name in Measured.Parts do
        if Measured.describe(Workspace:FindFirstChild(name) :: MeshPart) ~= theirs[name] then
            all = false
        end
    end
    if all then
        break
    end
    task.wait(0.1)
end

for _, name in Measured.Parts do
    local here = Measured.describe(Workspace:FindFirstChild(name) :: MeshPart)
    if here == theirs[name] then
        print(`[measured] {name} same {here}`)
    else
        print(`[measured] {name} DIFFERENT server={theirs[name]} client={here}`)
    end
end

local mine = Instance.new("MeshPart")
mine.Name = "Mine"
mine.Anchored = true
mine.CollisionFidelity = Enum.CollisionFidelity.Box
mine.Position = vector.create(-10, 2, 0)
mine.MeshContent = Measured.Mesh :: Content
mine.Parent = Workspace
print(`[measured] Mine {Measured.describe(mine)}`)

local bare = Workspace:FindFirstChild("Bare") :: MeshPart
bare.MeshContent = Measured.Mesh :: Content
print(`[measured] Bare {Measured.describe(bare)}`)

print("[measured] done")
game:Shutdown()
