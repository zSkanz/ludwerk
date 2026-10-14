--!strict
--- Light that changes every frame (ADR 0203): two `Dynamic` volumes of two
--- thousand points each, one near the camera and one far, and a torch between
--- them whose brightness is another every frame -- so each volume's light is
--- out of date the moment it has been worked out.
---
--- What `run_relight_gate.cmake` reads off this script's own account:
---
---   * both volumes are worked out, and say so (`Completed`, `IsReady`) --
---     the far one too, which a near one lit again without end once kept
---     from ever being lit at all;
---   * neither falls back from ready while it is lit again: being lit again
---     under a light that moved is upkeep;
---   * and `Refresh` is not ready from the line that calls it until the
---     volume is worked out anew, when `Completed` is said again.

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

Lighting.ClockTime = 15
Lighting.GeographicLatitude = 30

local function slab(name: string, size: vector, position: vector, color: Color3)
    local part = Instance.new("Part")
    part.Name = name
    part.Size = size
    part.Position = position
    part:SetMaterialParameter("Color", color)
    part.Anchored = true
    part.Parent = workspace
end
slab("Ground", vector.create(160, 1, 80), vector.create(20, -0.5, 0), Color3.new(0.5, 0.5, 0.45))
for index = 0, 19 do
    local x = (index % 10) * 7 - 12
    local z = (index // 10) * 10 - 5
    slab(`Wall{index}`, vector.create(3, 4, 1), vector.create(x, 2, z), Color3.new(0.7, 0.6, 0.5))
end

type Told = {
    Volume: IrradianceVolume,
    Completed: number,
    Fell: number,
    FellAt: number,
    WasReady: boolean,
    FirstFrame: number,
}
local frame = 0
local told: { Told } = {}
local function room(name: string, centre: vector)
    local box = Instance.new("Part")
    box.Name = name
    box.Size = vector.create(20, 5, 20)
    box.Position = centre
    box:SetMaterialParameter("Transparency", 1)
    box.CanCollide = false
    box.CanQuery = false
    box.Anchored = true
    box.Parent = workspace
    local volume = Instance.new("IrradianceVolume")
    volume.Spacing = 1
    volume.Bounces = 2
    volume.Dynamic = true
    volume.Parent = box
    local one: Told = { Volume = volume, Completed = 0, Fell = 0, FellAt = 0, WasReady = false, FirstFrame = 0 }
    volume.Completed:Connect(function()
        one.Completed += 1
        if one.FirstFrame == 0 then
            one.FirstFrame = frame
        end
    end)
    table.insert(told, one)
end
room("Near", vector.create(0, 2.5, 0))
room("Far", vector.create(36, 2.5, 0))

local holder = Instance.new("Part")
holder.Name = "Torch"
holder.Size = vector.create(0.2, 0.2, 0.2)
holder.Position = vector.create(18, 3, 0)
holder.Anchored = true
holder.CanCollide = false
holder.CanQuery = false
holder.Parent = workspace
local torch = Instance.new("PointLight")
torch.Range = 30
torch.Brightness = 3
torch.Parent = holder

local camera = Instance.new("Camera")
camera.FieldOfView = 70
camera.Parent = workspace
workspace.CurrentCamera = camera
camera.CFrame = CFrame.lookAt(vector.create(-9, 2, 8), vector.create(36, 2, 0))

-- Where the script's account ends: the frame the driver was told to stop at,
-- less a few for what is said to be heard.
local Refreshed = 260
local Last = 520
local refreshedReady: boolean? = nil
local completedBefore = 0

RunService.PreRender:Connect(function()
    frame += 1
    torch.Brightness = 3 + math.sin(frame / 7) * 1.5
    for _, one in told do
        local ready = one.Volume:IsReady()
        if one.WasReady and not ready and frame ~= Refreshed + 1 then
            one.Fell += 1
            one.FellAt = frame
        end
        one.WasReady = ready
    end
    if frame == Refreshed then
        -- Asked for again: not ready from this line, whatever was drawn.
        completedBefore = told[1].Completed
        told[1].Volume:Refresh()
        refreshedReady = told[1].Volume:IsReady()
        told[1].WasReady = false
    end
    if frame == Last then
        for index, one in told do
            print(
                `relight: volume {index} ready {one.Volume:IsReady()} progress {one.Volume:GetProgress()}`
                    .. ` first completed at frame {one.FirstFrame} completed {one.Completed} fell {one.Fell} (frame {one.FellAt})`
            )
        end
        print(
            `relight: refreshed, ready at once {refreshedReady}, completed again {told[1].Completed - completedBefore}`
        )
    end
end)
