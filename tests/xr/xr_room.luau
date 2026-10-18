--!strict
-- The player's room under the simulated headset (ADR 0247 §2): the recorded
-- room comes back from `GetRoomAsync` -- a floor, a ceiling, four walls and a
-- table -- and `CreateRoomParts` makes it solid, so a ball dropped on the
-- table rests on its top.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local camera = Instance.new("Camera")
camera.CFrame = CFrame.new(0, 0, 0)
camera.Parent = workspace
workspace.CurrentCamera = camera

local function fixed(value: number): string
    local text = string.format("%.2f", value)
    return if text == "-0.00" then "0.00" else text
end

task.spawn(function()
    while not XRService.Enabled do
        RunService.PostSimulation:Wait()
    end
    local room = XRService:GetRoomAsync()
    assert(room ~= nil, "the simulated headset knows a room")
    local counts: { [string]: number } = {}
    local tableTop = "?"
    for _, plane in room.Planes do
        counts[plane.Kind.Name] = (counts[plane.Kind.Name] or 0) + 1
        if plane.Kind == Enum.XRPlaneKind.Table then
            local up = plane.CFrame.LookVector
            tableTop = `{fixed(plane.CFrame.Position.y)} {fixed(plane.Size.x)}x{fixed(plane.Size.y)} `
                .. `facing={fixed(-up.y)}`
        end
    end
    local folder = XRService:CreateRoomParts()
    assert(folder ~= nil, "the room is made solid")
    local ball = Instance.new("Part")
    ball.Shape = Enum.PartShape.Ball
    ball.Size = vector.create(0.2, 0.2, 0.2)
    ball.Position = vector.create(0, 1.6, -0.8)
    ball.Parent = workspace
    for _ = 1, 90 do
        RunService.PostSimulation:Wait()
    end
    print(
        `[room] planes={#room.Planes} floor={counts.Floor} ceiling={counts.Ceiling} walls={counts.Wall} `
            .. `tables={counts.Table} table={tableTop} parts={#folder:GetChildren()} `
            .. `ball={fixed(ball.Position.y)}`
    )
end)
