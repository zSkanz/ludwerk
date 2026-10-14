--!strict
--- Conformance: a stamp that builds its parts from its parameters (ADR 0155 §8).
---
--- A `ModuleScript` named `Construct` under a stamp's root returns a function
--- of the copy, its parameters and a `Random` seeded from them, and builds what
--- it builds -- here a fence's posts, one per `Posts`. It runs when a copy is
--- placed and, in the editor, when a parameter is written; NOT in play, where a
--- construction script runs at construction only. The stamp is
--- `content/stamps/spec-fence.stamp.json`.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local function posts(fence: Instance): { Instance }
    local found = {}
    for _, child in fence:GetChildren() do
        if child.Name:sub(1, 4) == "Post" then
            table.insert(found, child)
        end
    end
    return found
end

describe("a stamp's Construct", function()
    it("builds the copy's parts from its parameters before the copy is handed over", function()
        local fence = Instance.stamp("spec-fence", true, { Posts = 5 })
        expect(#posts(fence)):ToBe(5)
        expect((fence:FindFirstChild("Post5") :: Part).Position.x):ToBeCloseTo(10, 0.0001)
        fence:Destroy()
    end)

    it("builds the same parts from the same parameters", function()
        local one = Instance.stamp("spec-fence", true, { Posts = 4 })
        local two = Instance.stamp("spec-fence", true, { Posts = 4 })
        for at = 1, 4 do
            local mine = (one:FindFirstChild(`Post{at}`) :: Instance):GetAttribute("Lean")
            local theirs = (two:FindFirstChild(`Post{at}`) :: Instance):GetAttribute("Lean")
            expect(mine):ToBe(theirs)
        end
        one:Destroy()
        two:Destroy()
    end)

    it("does not build again in play when a parameter is written", function()
        local fence = Instance.stamp("spec-fence", true, { Posts = 5 })
        fence.Parent = workspace
        fence:SetAttribute("Posts", 2)
        task.wait()
        task.wait()
        expect(#posts(fence)):ToBe(5)
        fence:Destroy()
    end)
end)
