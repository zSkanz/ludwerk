--!strict
--- Conformance: a script reads `Source` and never writes it (ADR 0137 §4, S0.2).
---
--- A script that could write another's `Source` and enable it would run text
--- the sandbox never loaded -- the `loadstring` the sandbox removed on purpose,
--- by another name. The editor, the scene reader and the dev server write it
--- through the engine's own path; a script only reads it.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

describe("Source", function()
    it("refuses a script's write to a Script's Source", function()
        local made = Instance.new("Script")
        made.Enabled = false
        expect(function()
            (made :: any).Source = "print('ran')"
        end):ToThrow("the editor writes it")
        expect(made.Source):ToBe("")
        made:Destroy()
    end)

    it("refuses a script's write to a ModuleScript's Source", function()
        local module = Instance.new("ModuleScript")
        expect(function()
            (module :: any).Source = "return 1"
        end):ToThrow("the editor writes it")
        expect(module.Source):ToBe("")
        module:Destroy()
    end)

    it("still reads it: a script's own code is there to read", function()
        expect(typeof(script.Source)):ToBe("string")
        expect(#script.Source > 0):ToBe(true)
    end)
end)
