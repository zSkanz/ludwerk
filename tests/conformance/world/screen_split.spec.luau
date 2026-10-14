--!strict
--- Several players at one screen (ADR 0205) as a script meets it: how the
--- window is shared, what a camera and an interface answer inside a view,
--- whose an interface is when its player leaves, and what
--- `@engine/splitscreen` writes as two players walk apart.
---
--- Which rectangle is whose is asserted in C++ (`engine_app_tests`, "ADR
--- 0205"); what is drawn is the picture gates' (`split_screen_*`); a match
--- with a guest is `netcode_guests`.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local NetworkService = game:GetService("NetworkService")
local UIService = game:GetService("UIService")

local splitscreen = require("@engine/splitscreen")

-- Two frames: the window's views are decided in a frame, from what the
-- scripts of the tick before it left.
local function settle()
    task.wait()
    task.wait()
end

local function camera(at: vector): Camera
    local made = Instance.new("Camera")
    made.FieldOfView = 60
    made.CFrame = CFrame.lookAt(at, at + vector.create(0, 0, -10))
    made.Parent = workspace
    return made
end

type Sofa = { First: Player, Guest: Player, Cameras: { Camera } }

-- The machine's own player and one guest, each with a camera.
local function sofa(): Sofa
    local first = NetworkService.LocalPlayer :: Player
    local guest = NetworkService:AddLocalPlayer() :: Player
    assert(guest ~= nil, "a guest sits down offline")
    local cameras = { camera(vector.create(0, 5, 0)), camera(vector.create(100, 5, 0)) }
    first.Camera = cameras[1]
    guest.Camera = cameras[2]
    return { First = first, Guest = guest, Cameras = cameras }
end

local function clear(seated: Sofa)
    workspace.ScreenSplit = Enum.ScreenSplit.None
    workspace.SplitBlend = 0
    workspace.SplitAngle = 0
    seated.First.Camera = nil
    NetworkService:RemoveLocalPlayer(seated.Guest)
    for _, each in seated.Cameras do
        each:Destroy()
    end
    settle()
end

describe("Workspace.ScreenSplit", function()
    it("is one picture until it is said otherwise, and keeps what it is given", function()
        expect(workspace.ScreenSplit):ToBe(Enum.ScreenSplit.None)
        expect(workspace.SplitBlend):ToBe(0)
        expect(workspace.SplitAngle):ToBe(0)
        expect(workspace.SplitLineThickness):ToBe(4)

        workspace.SplitAngle = 370
        expect(workspace.SplitAngle):ToBeCloseTo(10, 0.001)
        workspace.SplitAngle = 0
        workspace.SplitLineColor = Color3.new(1, 0, 0)
        expect(workspace.SplitLineColor.R):ToBe(1)
        workspace.SplitLineColor = Color3.new(0, 0, 0)

        expect(function()
            workspace.SplitBlend = 2
        end):ToThrow()
        expect(function()
            workspace.SplitLineThickness = -1
        end):ToThrow()
    end)

    it("gives a local player's camera its part of the window", function()
        local seated = sofa()
        local whole = seated.Cameras[1].ViewportSize
        workspace.ScreenSplit = Enum.ScreenSplit.Columns
        settle()

        -- Half the window each, the guest's on the right.
        expect(seated.Cameras[1].ViewportSize.X):ToBeCloseTo(whole.X / 2, 1)
        expect(seated.Cameras[2].ViewportSize.X):ToBeCloseTo(whole.X / 2, 1)
        expect(seated.Cameras[2].ViewportSize.Y):ToBeCloseTo(whole.Y, 1)

        -- A point straight ahead of the guest's camera is in the middle of
        -- the guest's part, in window pixels -- and a point of the window
        -- there is a ray straight out of that camera.
        local ahead = vector.create(100, 5, -20)
        local point, depth, onScreen = seated.Cameras[2]:WorldToViewportPoint(ahead)
        expect(onScreen):ToBe(true)
        expect(depth):ToBeCloseTo(20, 0.01)
        expect(point.X):ToBeCloseTo(whole.X * 0.75, 1)
        expect(point.Y):ToBeCloseTo(whole.Y / 2, 1)
        local origin, direction = seated.Cameras[2]:ViewportPointToRay(point)
        expect(origin.x):ToBeCloseTo(100, 0.01)
        expect(direction.z):ToBeCloseTo(-1, 0.001)

        -- One above another: half the height each.
        workspace.ScreenSplit = Enum.ScreenSplit.Rows
        settle()
        expect(seated.Cameras[2].ViewportSize.X):ToBeCloseTo(whole.X, 1)
        expect(seated.Cameras[2].ViewportSize.Y):ToBeCloseTo(whole.Y / 2, 1)

        -- One picture again: every camera answers for the window.
        workspace.ScreenSplit = Enum.ScreenSplit.None
        settle()
        expect(seated.Cameras[2].ViewportSize.X):ToBeCloseTo(whole.X, 1)
        clear(seated)
    end)

    it("takes a camera for a player's view and nothing else", function()
        local first = NetworkService.LocalPlayer :: Player
        local part = Instance.new("Part")
        expect(function()
            (first :: any).Camera = part
        end):ToThrow()
        expect(first.Camera):ToBeNil()
        expect(function()
            first.GamepadId = -1
        end):ToThrow()
        first.GamepadId = 3
        expect(first.GamepadId):ToBe(3)
        first.GamepadId = 0
        part:Destroy()
    end)
end)

describe("ScreenGui.Player", function()
    it("lays a player's interface out in that player's view", function()
        local seated = sofa()
        local whole = seated.Cameras[1].ViewportSize
        local hud = Instance.new("ScreenGui")
        hud.ScreenInsets = false
        hud.Player = seated.Guest
        hud.Parent = UIService
        local fill = Instance.new("Frame")
        fill.Size = UDim2.fromScale(1, 1)
        fill.Parent = hud

        -- One picture: the whole window.
        settle()
        expect(fill.AbsoluteSize.X):ToBeCloseTo(whole.X, 1)

        -- Side by side: the guest's half, on the right.
        workspace.ScreenSplit = Enum.ScreenSplit.Columns
        settle()
        expect(fill.AbsolutePosition.X):ToBeCloseTo(whole.X / 2, 1)
        expect(fill.AbsoluteSize.X):ToBeCloseTo(whole.X / 2, 1)
        expect(fill.AbsoluteSize.Y):ToBeCloseTo(whole.Y, 1)

        -- Everybody's again.
        hud.Player = nil
        settle()
        expect(fill.AbsolutePosition.X):ToBeCloseTo(0, 1)
        expect(fill.AbsoluteSize.X):ToBeCloseTo(whole.X, 1)
        hud:Destroy()
        clear(seated)
    end)

    it("has a selection of its own, of what is in it", function()
        local seated = sofa()
        local hud = Instance.new("ScreenGui")
        hud.Player = seated.Guest
        hud.Parent = UIService
        local card = Instance.new("TextButton")
        card.Size = UDim2.fromOffset(120, 40)
        card.Parent = hud
        local other = Instance.new("ScreenGui")
        other.Parent = UIService
        local elsewhere = Instance.new("TextButton")
        elsewhere.Size = UDim2.fromOffset(120, 40)
        elsewhere.Parent = other

        hud.SelectedObject = card
        expect(hud.SelectedObject):ToBe(card)
        -- Not everybody's selection, and nothing from another screen.
        expect(UIService.SelectedObject):ToBeNil()
        expect(function()
            hud.SelectedObject = elsewhere
        end):ToThrow()
        expect(hud.SelectedObject):ToBe(card)
        hud.SelectedObject = nil
        expect(hud.SelectedObject):ToBeNil()

        hud:Destroy()
        other:Destroy()
        clear(seated)
    end)

    it("is nobody's when its player leaves", function()
        local seated = sofa()
        local hud = Instance.new("ScreenGui")
        hud.Player = seated.Guest
        hud.Parent = UIService
        expect(hud.Player):ToBe(seated.Guest)
        clear(seated)
        expect(hud.Player):ToBeNil()
        -- And a player that is not at this machine cannot have one.
        hud.Player = NetworkService.LocalPlayer
        expect(hud.Player):ToBe(NetworkService.LocalPlayer)
        hud:Destroy()
    end)
end)

describe("@engine/splitscreen", function()
    it("is one picture while two are together, and a line across the way between them when they part", function()
        local first = NetworkService.LocalPlayer :: Player
        local guest = NetworkService:AddLocalPlayer() :: Player
        local bodies: { Part } = {}
        for index = 1, 2 do
            local body = Instance.new("Part")
            body.Anchored = true
            body.Position = vector.create(index * 2, 1, 0)
            body.Parent = workspace
            bodies[index] = body
        end

        local screen = splitscreen.follow({ SplitDistance = 20 })
        expect(workspace.ScreenSplit):ToBe(Enum.ScreenSplit.Dynamic)
        local own = screen:Add(first, bodies[1])
        screen:Add(guest, bodies[2])
        expect(first.Camera):ToBe(own)
        expect((guest.Camera :: Camera).ClassName):ToBe("Camera")

        -- Two metres apart: one picture, and each one's own camera looks
        -- where the one camera does -- the middle between them.
        screen:Update(1 / 60)
        settle()
        expect(workspace.SplitBlend):ToBe(0)
        expect(own.CFrame.Position.x):ToBeCloseTo(screen.Camera.CFrame.Position.x, 0.001)

        -- Forty apart, the second to the right: the line is whole, and
        -- upright -- the way from the first to the second is along the
        -- screen.
        bodies[2].Position = vector.create(42, 1, 0)
        screen:Update(1 / 60)
        settle()
        expect(workspace.SplitBlend):ToBe(1)
        expect(workspace.SplitAngle):ToBeCloseTo(0, 0.01)
        -- Each looks ten metres -- half the distance the screen parts at --
        -- from its own player towards the other.
        expect(own.CFrame.Position.x):ToBeCloseTo(2 + 10 + screen.Offset.x, 0.01)

        -- The second away from the camera, which is up the screen.
        bodies[2].Position = vector.create(2, 1, -40)
        screen:Update(1 / 60)
        settle()
        expect(workspace.SplitAngle):ToBeCloseTo(90, 0.01)

        -- Stopped: one picture, and the cameras it made are gone.
        screen:Destroy()
        expect(workspace.ScreenSplit):ToBe(Enum.ScreenSplit.None)
        expect(workspace.SplitBlend):ToBe(0)
        expect(first.Camera):ToBeNil()
        NetworkService:RemoveLocalPlayer(guest)
        for _, body in bodies do
            body:Destroy()
        end
        settle()
    end)
end)
