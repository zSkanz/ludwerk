--!strict
--- Conformance: the script environment (docs/api-design.md 2.2, 2.5, 3).
---
--- Section 3 makes two promises about the shape of running code: the mount is
--- a tree of `Script`s and `Folder`s under the script service its folder names
--- (ADR 0105: `src/client/` under `GlobalScriptService.Client`, where a spec
--- runs), and the `script` global is the running one of those.
---
--- **The third promise was reversed by the human at E3** and this file is where
--- that shows. It read "modules are files reached by string -- there is no
--- `ModuleScript` class and no `require(instance)`", which followed from a
--- script being a FILE. A script's source is a PROPERTY now (ADR 0050): a
--- script is an ordinary instance, a `ModuleScript` is one that is required
--- rather than run, and `require(module)` evaluates it once.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local beforeEach = testing.beforeEach

describe("the script global", function()
    it("is a Script instance", function()
        expect(script).Never:ToBeNil()
        expect(script:IsA("Script")):ToBe(true)
    end)

    it("reports Script as its ClassName", function()
        expect(script.ClassName):ToBe("Script")
    end)

    it("has a non-empty Name", function()
        expect(script.Name):ToBeA("string")
        expect(#script.Name > 0):ToBe(true)
    end)

    it("is a descendant of the DataModel", function()
        expect(script:IsDescendantOf(game)):ToBe(true)
    end)

    it("is mounted under GlobalScriptService.Client", function()
        local global = game:GetService("GlobalScriptService")
        expect(script:IsDescendantOf(global:FindFirstChild("Client") :: Instance)):ToBe(true)
    end)

    it("is enabled while it runs", function()
        expect(script.Enabled):ToBeA("boolean")
        expect(script.Enabled):ToBe(true)
    end)

    it("is not the DataModel or a service", function()
        expect(script).Never:ToBe(game)
        expect(script).Never:ToBe(workspace)
        expect(script).Never:ToBe(game:GetService("GlobalScriptService"))
    end)

    -- Section 3 points script authors at attributes for per-script config, so
    -- the running Script has to accept them like any other instance.
    it("carries attributes like any other instance", function()
        expect(script:GetAttribute("ConformanceProbe")):ToBeNil()
        script:SetAttribute("ConformanceProbe", 3)
        expect(script:GetAttribute("ConformanceProbe")):ToBe(3)
        script:SetAttribute("ConformanceProbe", nil)
        expect(script:GetAttribute("ConformanceProbe")):ToBeNil()
    end)

    it("carries tags like any other instance", function()
        expect(script:HasTag("ConformanceProbeTag")):ToBe(false)
        script:AddTag("ConformanceProbeTag")
        expect(script:HasTag("ConformanceProbeTag")):ToBe(true)
        script:RemoveTag("ConformanceProbeTag")
        expect(script:HasTag("ConformanceProbeTag")):ToBe(false)
    end)
end)

describe("GlobalScriptService", function()
    local global: GlobalScriptService
    local client: Instance

    beforeEach(function()
        global = game:GetService("GlobalScriptService")
        client = global:FindFirstChild("Client") :: Instance
    end)

    it("is a singleton child of game", function()
        expect(game:GetService("GlobalScriptService")):ToBe(global)
        expect(global.Parent):ToBe(game)
        expect(global.ClassName):ToBe("GlobalScriptService")
    end)

    -- ADR 0105: three fixed folders, whose place decides where their code runs.
    it("holds the three fixed folders Server, Client and Shared", function()
        for _, name in { "Server", "Client", "Shared" } do
            local folder = global:FindFirstChild(name)
            expect(folder).Never:ToBeNil()
            expect((folder :: Instance).ClassName):ToBe("Folder")
        end
    end)

    it("refuses to rename, move or destroy a fixed folder", function()
        local shared = global:FindFirstChild("Shared") :: Instance
        expect(function()
            shared.Name = "Common"
        end):ToThrow()
        expect(function()
            shared.Parent = workspace
        end):ToThrow()
        expect(function()
            shared:Destroy()
        end):ToThrow()
        expect(shared.Name):ToBe("Shared")
        expect(shared.Parent):ToBe(global)
    end)

    it("holds at least the running script under Client", function()
        expect(#client:GetDescendants() > 0):ToBe(true)
    end)

    -- Section 3: every mounted file is a `Script` and every source
    -- subdirectory is a `Folder`. Nothing else has a reason to be in the
    -- mount, so anything else there is a mounting bug.
    it("contains only Script and Folder instances under Client", function()
        for _, descendant in client:GetDescendants() do
            local className = descendant.ClassName
            expect(className == "Script" or className == "Folder"):ToBe(true)
        end
    end)

    it("gives every mounted Script an Enabled boolean", function()
        for _, descendant in client:GetDescendants() do
            if descendant:IsA("Script") then
                -- `IsA` answers a boolean rather than narrowing the type, so the cast
                -- is what a caller writes here too.
                expect((descendant :: Script).Enabled):ToBeA("boolean")
            end
        end
    end)

    it("names every mounted instance", function()
        for _, descendant in client:GetDescendants() do
            expect(descendant.Name):ToBeA("string")
            expect(#descendant.Name > 0):ToBe(true)
        end
    end)
end)

describe("the scene's script services", function()
    it("are two singleton children of game", function()
        expect(game:GetService("ServerScriptService").Parent):ToBe(game)
        expect(game:GetService("ClientScriptService").Parent):ToBe(game)
    end)

    it("have replaced ScriptService, which is refused by name", function()
        expect(function()
            (game :: any):GetService("ScriptService")
        end):ToThrow()
    end)
end)

describe("the module model", function()
    -- **This block asserted the opposite of all of it until E3**, and the
    -- reversal is the human's: a script's source is a PROPERTY, so a script is
    -- an ordinary instance -- made like any other, put wherever you want it,
    -- and saved inside whatever holds it. What was true before came from a
    -- script being a FILE, which it no longer is (ADR 0050).

    -- **The modules' code comes from a stamp** (ADR 0137 §4): a script cannot
    -- write `Source`, so each case takes a module the engine read from
    -- `content/stamps/spec-modules.stamp.json`.
    local function fixture(name: string): Instance
        return (Instance.stamp("spec-modules"):FindFirstChild(name) :: Instance):Clone()
    end

    it("makes a ModuleScript, and requiring it gives back what it returns", function()
        local module = fixture("Answer")
        module.Name = "Shared"

        local required = (require :: any)(module)
        expect(typeof(required)):ToBe("table")
        expect(required.answer):ToBe(42)

        -- **One evaluation per module**, which is the whole of what a module
        -- is: the second require is the same table rather than a second one.
        expect((require :: any)(module)):ToBe(required)
    end)

    it("requires a module that is a child of a Script", function()
        -- **The shape every project has.** A script keeps its own helpers
        -- beside it, reaches them through `script.Name`, and never has to know
        -- where in the tree it was mounted.
        local module = fixture("Seven")
        module.Name = "Nested"
        module.Parent = script

        local required = (require :: any)(script:FindFirstChild("Nested"))
        expect(typeof(required)):ToBe("table")
        expect(required.answer):ToBe(7)
    end)

    it("requires a module that is a child of another module", function()
        -- **`script` inside a module is the MODULE** (D111). It used to be
        -- whatever required it -- `luaL_sandboxthread` chains the new globals
        -- table to the creating thread's -- so a nested module looked for its
        -- own children under somebody else's instance and found none.
        -- The outer one reaches its own child, `Inner`, the same way a Script
        -- does, which is what makes a module a place to put things rather
        -- than a leaf.
        local outer = fixture("Outer")
        outer.Parent = script

        local required = (require :: any)(outer)
        expect(required.inner.depth):ToBe(2)
    end)

    it("requires a module that is inside a Folder", function()
        local folder = Instance.new("Folder")
        folder.Name = "Lib"
        folder.Parent = script

        local module = fixture("FromFolder")
        module.Name = "Helper"
        module.Parent = folder

        local lib = script:FindFirstChild("Lib") :: Instance
        local required = (require :: any)(lib:FindFirstChild("Helper"))
        expect(required.from):ToBe("folder")
    end)

    it("refuses require of a Script, because a Script runs", function()
        local runner = fixture("ReturnsOne")

        -- Requiring one would run it a second time somewhere else, which is
        -- exactly what the two classes exist to keep apart.
        expect(function()
            return (require :: any)(runner)
        end):ToThrow()
    end)

    it("propagates a module's error, and caches the failure", function()
        local broken = fixture("Broken")

        expect(function()
            return (require :: any)(broken)
        end):ToThrow()
        -- Cached rather than retried: a module that fails stays failed, which
        -- is what stops one broken module being re-evaluated per requirer.
        expect(function()
            return (require :: any)(broken)
        end):ToThrow()
    end)

    it("refuses require of an unknown module path", function()
        local requireByValue = require :: any
        expect(function()
            requireByValue("@engine/there-is-no-such-module")
        end):ToThrow()
    end)

    it("returns the same table for two requires of one module", function()
        expect(require("@engine/testing")):ToBe(testing)
    end)
end)
