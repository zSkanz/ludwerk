--!strict
--[[
	The authority: three `MeshPart`s made by script, each meeting its mesh a
	way -- named with nothing else set, named after its `Size` was, and
	naming a mesh the content does not hold -- and a fourth that names none,
	for the joiner to name one on. Answers what it holds of each.
]]

local NetworkService = game:GetService("NetworkService")
local Workspace = game:GetService("Workspace")

local Measured = require("@shared/Measured")

local function make(name: string, at: number): MeshPart
    local part = Instance.new("MeshPart")
    part.Name = name
    part.Anchored = true
    part.CollisionFidelity = Enum.CollisionFidelity.Box
    part.Position = vector.create(at, 2, 0)
    return part
end

local served = make("Served", 0)
served.MeshContent = Measured.Mesh :: Content
served.Parent = Workspace

local sized = make("Sized", 10)
sized.Size = vector.create(3, 3, 3)
sized.MeshContent = Measured.Mesh :: Content
sized.Parent = Workspace

local missing = make("Missing", 20)
missing.MeshContent = "asset://models/none.gltf" :: Content
missing.Parent = Workspace

local bare = make("Bare", 30)
bare.Parent = Workspace

for _, name in Measured.Parts do
    print(`[measured] server {name} {Measured.describe(Workspace:FindFirstChild(name) :: MeshPart)}`)
end

local held = Instance.new("RemoteFunction")
held.Name = "Held"
held.OnServerInvoke = function(): { [string]: string }
    local lines: { [string]: string } = {}
    for _, name in { "Served", "Sized", "Missing", "Bare" } do
        lines[name] = Measured.describe(Workspace:FindFirstChild(name) :: MeshPart)
    end
    return lines
end
held.Parent = Workspace

-- The joiner leaves when it has said what it saw; there is nobody else.
NetworkService.PlayerRemoving:Connect(function()
    game:Shutdown()
end)
