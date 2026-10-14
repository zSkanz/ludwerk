--!strict
--- Conformance: a stamp preloaded, and a pool of its copies (ADR 0155 §10).
---
--- `ContentProvider:PreloadAsync` takes a stamp by the name `Instance.stamp`
--- takes, and `@engine/stamppool` keeps copies of one made ahead, handing one
--- over at once and replacing what is given back with a fresh one -- so what
--- `Take` hands over is always the stamp as it is. The stamp is
--- `content/stamps/spec-gate.stamp.json`.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local ContentProvider = game:GetService("ContentProvider")

local function frames(count: number)
    for _ = 1, count do
        task.wait()
    end
end

describe("ContentProvider:PreloadAsync with a stamp", function()
    it("reports a stamp it read as a success, and one that is not there as a failure", function()
        local statuses: { [string]: EnumItem } = {}
        ContentProvider:PreloadAsync({ "spec-gate", "no-such-stamp" }, function(item, status)
            statuses[item :: string] = status
        end)
        frames(2)
        expect(statuses["spec-gate"]):ToBe(Enum.AssetFetchStatus.Success)
        expect(statuses["no-such-stamp"]):ToBe(Enum.AssetFetchStatus.Failure)
    end)
end)

describe("@engine/stamppool", function()
    it("fills itself, hands copies over with its parameters, and replaces what is given back", function()
        local StampPool = require("@engine/stamppool")
        local pool = StampPool.new("spec-gate", 2, { Width = 4 })
        frames(6)
        expect(pool:Ready()):ToBe(2)

        local holder = Instance.new("Folder")
        holder.Parent = workspace
        local copy = pool:Take(holder)
        expect(copy.Parent):ToBe(holder)
        expect(copy:GetAttribute("Width")):ToBe(4)
        expect((copy:FindFirstChild("Rail") :: Part).Size.x):ToBeCloseTo(4, 0.0001)

        -- Given back changed, it is not handed out again: it is gone, and a
        -- fresh copy takes its place.
        copy:SetAttribute("Width", 9)
        pool:Return(copy)
        expect(copy:IsDescendantOf(game)):ToBe(false)
        frames(6)
        expect(pool:Ready()):ToBe(2)
        local next = pool:Take(holder)
        expect(next:GetAttribute("Width")):ToBe(4)

        pool:Destroy()
        holder:Destroy()
    end)
end)
