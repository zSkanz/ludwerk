--!strict
--- `SubWorld`, from ADR 0107 §3: a scene running beside this one, in a world
--- of its own. What these cases check is the simulation half -- loading,
--- isolation, messages both ways, input, the budget, pausing and unloading. The
--- scene they run is `content/scenes/sub-world-spec.scene.json`, whose one
--- script reports what it sees and echoes what it is sent.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local SceneService = game:GetService("SceneService")

local Scene = "scenes/sub-world-spec.scene.json"

type Message = { n: number, [number]: any }

-- Up to a second of ticks for `check` to come true.
local function waitUntil(check: () -> boolean): boolean
    for _ = 1, 60 do
        if check() then
            return true
        end
        task.wait()
    end
    return check()
end

local function cabinet(): (SubWorld, { Message })
    local sub = Instance.new("SubWorld")
    sub.Scene = Scene
    sub.Parent = workspace
    local received: { Message } = {}
    sub.Received:Connect(function(...: any)
        table.insert(received, table.pack(...) :: any)
    end)
    return sub, received
end

local function find(received: { Message }, kind: string): Message?
    for _, message in received do
        if message[1] == kind then
            return message
        end
    end
    return nil
end

local function started(sub: SubWorld, received: { Message }): Message
    sub:Load()
    expect(waitUntil(function()
        return find(received, "started") ~= nil
    end)):ToBe(true)
    return find(received, "started") :: Message
end

describe("SubWorld", function()
    it("starts unloaded, with a view's defaults", function()
        local sub = Instance.new("SubWorld")
        expect(sub.Running):ToBe(true)
        expect(sub.Resolution):ToBe(Vector2.new(512, 288))
        expect(sub.UpdateInterval):ToBe(1)
        expect(sub.Quality):ToBe(Enum.ViewQuality.Simple)
        expect(sub:IsLoaded()):ToBe(false)
        sub:Destroy()
    end)

    it("is not in a sub-world here, so there is no host to send to", function()
        expect(SceneService:IsSubWorld()):ToBe(false)
        expect(function()
            SceneService:SendToHost("nobody")
        end):ToThrow()
    end)

    it("runs its scene in a world of its own, which cannot see this one", function()
        local marker = Instance.new("Part")
        marker.Name = "HostMarker"
        marker.Parent = workspace
        local sub, received = cabinet()
        local loaded = false
        sub.Loaded:Connect(function()
            loaded = true
        end)

        local hello = started(sub, received)
        expect(waitUntil(function()
            return loaded
        end)):ToBe(true)
        expect(sub:IsLoaded()):ToBe(true)
        -- Inside, it knows it is a sub-world, it cannot find this world's
        -- marker, it could not load a sub-world of its own, and its scene is
        -- the one it was given.
        expect(hello[2]):ToBe(true)
        expect(hello[3]):ToBe(false)
        expect(hello[4]):ToBe(false)
        expect(hello[5]):ToBe(Scene)
        -- And nothing of its world is in this one.
        expect(workspace:FindFirstChild("Cabinet")):ToBe(nil)

        sub:Destroy()
        marker:Destroy()
    end)

    it("carries plain values in and back out", function()
        local sub, received = cabinet()
        started(sub, received)
        sub:Send("ping", 42, { answer = "yes" }, vector.create(3, 4, 5))
        expect(waitUntil(function()
            return find(received, "echo") ~= nil
        end)):ToBe(true)
        local echo = find(received, "echo") :: Message
        expect(echo[2]):ToBe("ping")
        expect(echo[3]):ToBe(42)
        expect(echo[4]):ToEqual({ answer = "yes" })
        expect(echo[5]):ToBe(vector.create(3, 4, 5))
        -- Never an instance: the two worlds have none in common.
        expect(function()
            sub:Send(workspace)
        end):ToThrow()
        sub:Destroy()
    end)

    it("gives it only the input this world passes on", function()
        local sub, received = cabinet()
        started(sub, received)
        sub:SetInputState("Jump", true)
        expect(waitUntil(function()
            return find(received, "jump") ~= nil
        end)):ToBe(true)
        sub:SetInputState("Move", Vector2.new(1, 0))
        expect(waitUntil(function()
            return find(received, "move") ~= nil
        end)):ToBe(true)
        local moved = find(received, "move") :: Message
        expect(moved[2]):ToBe(1)
        expect(moved[3]):ToBe(0)
        expect(function()
            sub:SetInputState("Move", "sideways" :: any)
        end):ToThrow()
        sub:Destroy()
    end)

    it("holds its messages while it is paused", function()
        local sub, received = cabinet()
        started(sub, received)
        sub.Running = false
        sub:Send("held")
        for _ = 1, 5 do
            task.wait()
        end
        expect(find(received, "echo")):ToBe(nil)
        sub.Running = true
        expect(waitUntil(function()
            return find(received, "echo") ~= nil
        end)):ToBe(true)
        sub:Destroy()
    end)

    it("runs two at once, and refuses a third", function()
        local first, firstReceived = cabinet()
        local second, secondReceived = cabinet()
        local third = cabinet()
        started(first, firstReceived)
        started(second, secondReceived)
        expect(function()
            third:Load()
        end):ToThrow("max_sub_worlds")
        -- Unloading one frees its place.
        first:Unload()
        third:Load()
        expect(waitUntil(function()
            return third:IsLoaded() and not first:IsLoaded()
        end)):ToBe(true)
        first:Destroy()
        second:Destroy()
        third:Destroy()
    end)

    it("unloads, and loads again from the start", function()
        local sub, received = cabinet()
        started(sub, received)
        sub:Unload()
        expect(waitUntil(function()
            return not sub:IsLoaded()
        end)):ToBe(true)
        table.clear(received)
        started(sub, received)
        expect(sub:IsLoaded()):ToBe(true)
        sub:Destroy()
    end)

    it("says so when its scene is not there", function()
        local sub = Instance.new("SubWorld")
        sub.Scene = "scenes/not-a-scene.scene.json"
        sub.Parent = workspace
        sub:Load()
        for _ = 1, 3 do
            task.wait()
        end
        expect(sub:IsLoaded()):ToBe(false)
        sub:Destroy()
    end)
end)
