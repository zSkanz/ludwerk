--!strict
--- Conformance: `RemoteEvent` and `RemoteFunction`, played solo (N2, ADRs 0077 and 0079).
---
--- **One script runs solo and networked.** Solo, this machine is its own
--- authority and its own client, so every message is delivered here -- which
--- is what lets these cases exercise the whole of the script side: what
--- travels, what arrives, what is refused, and who the sender is.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local NetworkService = game:GetService("NetworkService")
local PhysicsService = game:GetService("PhysicsService")

local function step(ticks: number)
    task.wait(ticks * PhysicsService.FixedTimestep)
end

describe("remote events", function()
    it("delivers FireServer to the authority's own ServerReceived, from the local player", function()
        local remote = Instance.new("RemoteEvent")
        remote.Parent = workspace
        local crate = Instance.new("Part")
        crate.Parent = workspace

        local from: any = nil
        local received: { any }? = nil
        remote.ServerReceived:Connect(function(player, ...)
            from = player
            received = { ... }
        end)
        remote:FireServer("hello", 42, vector.create(1, 2, 3), { nested = { list = { 1, 2, 3 } }, crate = crate }, true)
        step(2)

        expect(from):ToBe(NetworkService.LocalPlayer)
        expect(received ~= nil):ToBe(true)
        if received then
            expect(received[1]):ToBe("hello")
            expect(received[2]):ToBe(42)
            expect(received[3]):ToBe(vector.create(1, 2, 3))
            expect(received[4].nested.list[3]):ToBe(3)
            -- An instance arrives as this machine's own copy: here, itself.
            expect(received[4].crate):ToBe(crate)
            expect(received[5]):ToBe(true)
        end
        remote:Destroy()
        crate:Destroy()
    end)

    it("carries a colour, a place, a 2D point, a layout size and an enum item (G12)", function()
        local remote = Instance.new("RemoteEvent")
        remote.Parent = workspace
        local received: { any }? = nil
        remote.ServerReceived:Connect(function(_, ...)
            received = { ... }
        end)
        local place = CFrame.new(1, 2, 3) * CFrame.fromEuler(0, math.pi / 2, 0)
        remote:FireServer(
            Color3.new(0.25, 0.5, 1),
            place,
            Vector2.new(4, 5),
            UDim.new(0.5, 10),
            UDim2.new(0.25, 1, 0.75, 2),
            Enum.PartShape.Ball,
            { tint = Color3.new(1, 0, 0) }
        )
        step(2)
        expect(received ~= nil):ToBe(true)
        if received then
            expect(received[1]):ToBe(Color3.new(0.25, 0.5, 1))
            expect(vector.magnitude(received[2].Position - place.Position) < 1e-6):ToBe(true)
            expect(vector.magnitude(received[2].LookVector - place.LookVector) < 1e-6):ToBe(true)
            expect(received[3]):ToBe(Vector2.new(4, 5))
            expect(received[4]):ToBe(UDim.new(0.5, 10))
            expect(received[5]):ToBe(UDim2.new(0.25, 1, 0.75, 2))
            expect(received[6]):ToBe(Enum.PartShape.Ball)
            expect(received[7].tint):ToBe(Color3.new(1, 0, 0))
        end
        remote:Destroy()
    end)

    it("delivers FireClient and FireAllClients to the local player's ClientReceived", function()
        local remote = Instance.new("RemoteEvent")
        remote.Parent = workspace
        local messages: { string } = {}
        remote.ClientReceived:Connect(function(text)
            table.insert(messages, text)
        end)
        remote:FireClient(NetworkService.LocalPlayer :: Player, "just you")
        remote:FireAllClients("everyone")
        step(2)
        expect(#messages):ToBe(2)
        expect(messages[1]):ToBe("just you")
        expect(messages[2]):ToBe("everyone")
        remote:Destroy()
    end)

    it("refuses at the call what cannot travel", function()
        local remote = Instance.new("RemoteEvent")
        remote.Parent = workspace
        local anyRemote = remote :: any
        expect(function()
            anyRemote:FireServer(function() end)
        end):ToThrow()
        local selfish: { [string]: any } = {}
        selfish.me = selfish
        expect(function()
            anyRemote:FireServer(selfish)
        end):ToThrow()
        expect(function()
            anyRemote:FireServer({ [{}] = 1 })
        end):ToThrow()
        expect(function()
            anyRemote:FireServer(string.rep("x", 70000))
        end):ToThrow()
        expect(function()
            anyRemote:FireClient(remote, "not a player")
        end):ToThrow()
        remote:Destroy()
    end)
end)

describe("unreliable remote events", function()
    it("delivers to this machine's own listeners, as a reliable event does", function()
        local fast = Instance.new("UnreliableRemoteEvent")
        fast.Parent = workspace
        local from: any = nil
        local up: { any } = {}
        local down: { any } = {}
        fast.ServerReceived:Connect(function(player, where, health)
            from = player
            up = { where, health }
        end)
        fast.ClientReceived:Connect(function(text)
            table.insert(down, text)
        end)
        fast:FireServer(vector.create(4, 5, 6), 80)
        fast:FireClient(NetworkService.LocalPlayer :: Player, "just you")
        fast:FireAllClients("everyone")
        step(2)
        -- No wire to cross, so nothing is lost: one script runs solo and in a match.
        expect(from):ToBe(NetworkService.LocalPlayer)
        expect(up[1]):ToBe(vector.create(4, 5, 6))
        expect(up[2]):ToBe(80)
        expect(#down):ToBe(2)
        expect(down[1]):ToBe("just you")
        expect(down[2]):ToBe("everyone")
        fast:Destroy()
    end)

    it("carries a buffer, byte for byte", function()
        local fast = Instance.new("UnreliableRemoteEvent")
        fast.Parent = workspace
        local packed = buffer.create(12)
        buffer.writeu16(packed, 0, 513)
        buffer.writef32(packed, 2, 1.5)
        buffer.writei8(packed, 11, -7)
        local got: buffer? = nil
        local inside: buffer? = nil
        fast.ClientReceived:Connect(function(bytes: buffer, more: { part: buffer })
            got = bytes
            inside = more.part
        end)
        fast:FireAllClients(packed, { part = buffer.create(3) })
        step(2)
        expect(got ~= nil):ToBe(true)
        if got and inside then
            -- A copy, as every value that crosses is.
            expect(got ~= packed):ToBe(true)
            expect(buffer.len(got)):ToBe(12)
            expect(buffer.readu16(got, 0)):ToBe(513)
            expect(buffer.readf32(got, 2)):ToBe(1.5)
            expect(buffer.readi8(got, 11)):ToBe(-7)
            expect(buffer.len(inside)):ToBe(3)
        end
        fast:Destroy()
    end)

    it("keeps nothing for a listener that connects later", function()
        local fast = Instance.new("UnreliableRemoteEvent")
        fast.Parent = workspace
        fast:FireAllClients("too early")
        step(2)
        local heard: { string } = {}
        fast.ClientReceived:Connect(function(text)
            table.insert(heard, text)
        end)
        step(2)
        -- A reliable event keeps the call for its first listener; this one is gone.
        expect(#heard):ToBe(0)
        fast:FireAllClients("now")
        step(2)
        expect(#heard):ToBe(1)
        expect(heard[1]):ToBe("now")
        fast:Destroy()
    end)

    it("refuses a message over sixteen kibibytes, and what cannot travel", function()
        local fast = Instance.new("UnreliableRemoteEvent")
        fast.Parent = workspace
        local anyFast = fast :: any
        expect(function()
            anyFast:FireServer(string.rep("x", 17000))
        end):ToThrow()
        expect(function()
            anyFast:FireServer(function() end)
        end):ToThrow()
        -- Under the limit it goes.
        anyFast:FireServer(string.rep("x", 15000))
        fast:Destroy()
    end)

    it("counts what it sends in GetStats", function()
        local stats = NetworkService:GetStats()
        expect(type(stats.UnreliableSent)):ToBe("number")
        expect(type(stats.UnreliableReceived)):ToBe("number")
        expect(type(stats.UnreliableDropped)):ToBe("number")
    end)
end)

describe("remote functions", function()
    it("answers InvokeServerAsync with what OnServerInvoke returned, as the local player", function()
        local remote = Instance.new("RemoteFunction")
        remote.Parent = workspace
        local asker: any = nil
        remote.OnServerInvoke = function(player: Player, item: string, count: number)
            asker = player
            return `{item} x{count}`, { total = count * 2 }
        end
        local label, detail = remote:InvokeServerAsync("sword", 3)
        expect(asker):ToBe(NetworkService.LocalPlayer)
        expect(label):ToBe("sword x3")
        expect(detail.total):ToBe(6)
        remote:Destroy()
    end)

    it("reads back the handler it was given, and forgets it on nil", function()
        local remote = Instance.new("RemoteFunction")
        local handler = function(_player: Player)
            return 1
        end
        remote.OnServerInvoke = handler
        expect(remote.OnServerInvoke):ToBe(handler)
        remote.OnServerInvoke = nil
        expect(remote.OnServerInvoke):ToBe(nil)
        expect(function()
            (remote :: any).OnServerInvoke = 42
        end):ToThrow("net.err.remote_handler")
        remote:Destroy()
    end)

    it("waits for a handler that yields", function()
        local remote = Instance.new("RemoteFunction")
        remote.Parent = workspace
        remote.OnServerInvoke = function(_player: Player, ticks: number)
            step(ticks)
            return "late"
        end
        expect(remote:InvokeServerAsync(3)):ToBe("late")
        remote:Destroy()
    end)

    it("raises at the caller when the handler fails, or there is none", function()
        local remote = Instance.new("RemoteFunction")
        remote.Parent = workspace
        remote.OnServerInvoke = function(_player: Player)
            error("out of swords")
        end
        local ok, message = pcall(function()
            return remote:InvokeServerAsync()
        end)
        expect(ok):ToBe(false)
        expect(string.find(tostring(message), "net.err.remote_invoke_failed", 1, true) ~= nil):ToBe(true)
        expect(string.find(tostring(message), "out of swords", 1, true) ~= nil):ToBe(true)

        -- A handler whose answer cannot travel fails the same way.
        remote.OnServerInvoke = function(_player: Player)
            return function() end
        end
        local sent = pcall(function()
            return remote:InvokeServerAsync()
        end)
        expect(sent):ToBe(false)

        remote.OnServerInvoke = nil
        local answered, missing = pcall(function()
            return remote:InvokeServerAsync()
        end)
        expect(answered):ToBe(false)
        expect(string.find(tostring(missing), "net.err.remote_no_handler", 1, true) ~= nil):ToBe(true)
        remote:Destroy()
    end)
end)
