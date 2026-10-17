--!strict
--- `RunService.TimeDilation` (ADR 0233). The suite runs alone, where nothing
--- slows the clock: the rate is 1 and a script cannot set it. How a server
--- slows, and its replicas with it, is asserted in C++ (`engine_app_tests`,
--- `engine_replication_tests`).

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect

local RunService = game:GetService("RunService")

describe("TimeDilation", function()
    it("is 1 in a world nothing slows", function()
        expect(RunService.TimeDilation):ToBe(1)
    end)

    it("is the engine's to say, not a script's", function()
        local ok = pcall(function()
            (RunService :: any).TimeDilation = 0.5
        end)
        expect(ok):ToBe(false)
        expect(RunService.TimeDilation):ToBe(1)
    end)
end)
