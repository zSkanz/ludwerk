--!strict
--- Conformance: a script runs exactly while it is live (ADR 0137 §1, S1).
---
--- Live is: enabled, with code, under the `DataModel`, under no inert storage
--- (`ServerStorage`, `ReplicatedStorage`), its side running here. Every way a
--- script comes into the world or leaves it starts or stops it -- before this,
--- a start happened only at boot, at a scene change or on an `Enabled` write,
--- so a clone, a stamp and a model parented at run time never ran, and a
--- script that left the world went on running.
---
--- The scripts' code comes from `content/stamps/spec-live.stamp.json`,
--- because a script cannot write `Source` (ADR 0137 §4). Each template there
--- is enabled; a clone is made unparented, which is not live, and parented
--- when the case wants it to start.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local beforeEach, afterEach = testing.beforeEach, testing.afterEach

local sandbox: Folder? = nil
local ticks: Folder? = nil

local function folder(): Folder
    return assert(sandbox, "the sandbox folder is created in beforeEach")
end

local function tickFolder(): Folder
    return assert(ticks, "the ticks folder is created in beforeEach")
end

local function template(name: string): Instance
    return (Instance.stamp("spec-live"):FindFirstChild(name) :: Instance):Clone()
end

--- A `Maker` clone, unparented, that will make a part named `made`.
local function maker(made: string): Script
    local script = template("Maker") :: Script
    script:SetAttribute("Make", made)
    return script
end

--- The parts made so far, joined, in the order they were parented.
local function made(): string
    local names = {}
    for _, child in folder():GetChildren() do
        if child.ClassName == "Part" then
            table.insert(names, child.Name)
        end
    end
    return table.concat(names, ",")
end

describe("a live script", function()
    beforeEach(function()
        local created = Instance.new("Folder")
        created.Name = "ScriptLiveSpec"
        created.Parent = workspace
        sandbox = created
        local counted = Instance.new("Folder")
        counted.Name = "ScriptLiveTicks"
        counted.Parent = workspace
        ticks = counted
    end)

    afterEach(function()
        folder():Destroy()
        tickFolder():Destroy()
        sandbox = nil
        ticks = nil
    end)

    it("starts when a clone of it is parented into the world", function()
        local script = maker("FromClone")
        task.wait()
        expect(made()):ToBe("")

        script.Parent = folder()
        task.wait()
        expect(made()):ToBe("FromClone")
        -- Once: parenting again elsewhere in the world is a move, not a start.
        script.Parent = workspace
        task.wait()
        expect(made()):ToBe("FromClone")
        script:Destroy()
    end)

    it("starts scripts moved in one tick in the order they were moved", function()
        -- Deterministic without a walk of the world (R10): the order of the
        -- moves, whatever the order of the instances in the tree.
        local early = Instance.new("Folder")
        early.Parent = folder()
        local late = Instance.new("Folder")
        late.Parent = folder()
        task.wait()
        -- In the tree `early`'s script comes first; it is moved second.
        maker("MovedFirst").Parent = late
        maker("MovedSecond").Parent = early
        task.wait()
        expect(made()):ToBe("MovedFirst,MovedSecond")
    end)

    it("starts when a model holding it is parented, and when a stamp is placed", function()
        local crate = template("Crate")
        task.wait()
        expect(made()):ToBe("")
        crate.Parent = folder()
        task.wait()
        expect(made()):ToBe("FromCrate")
    end)

    it("stops when it leaves the world, and starts afresh when it comes back", function()
        local script = template("Ticker") :: Script
        script.Parent = folder()
        task.wait()
        task.wait()
        local whileIn = #tickFolder():GetChildren()
        expect(whileIn > 0):ToBe(true)

        script.Parent = nil
        task.wait()
        task.wait()
        task.wait()
        expect(#tickFolder():GetChildren()):ToBe(whileIn)

        -- Back: a fresh run, alone -- the old run's handler does not come back
        -- beside it, so one heartbeat makes one part.
        script.Parent = folder()
        task.wait()
        local settled = #tickFolder():GetChildren()
        task.wait()
        expect(#tickFolder():GetChildren() - settled):ToBe(1)
        script:Destroy()
    end)

    it("stops when a model holding it moves out of the world", function()
        local holder = Instance.new("Model")
        holder.Parent = folder()
        local script = template("Ticker") :: Script
        script.Parent = holder
        task.wait()
        task.wait()
        local whileIn = #tickFolder():GetChildren()
        expect(whileIn > 0):ToBe(true)

        holder.Parent = nil
        task.wait()
        task.wait()
        expect(#tickFolder():GetChildren()):ToBe(whileIn)
        holder:Destroy()
    end)

    it("does not go on past a wait once it has stopped (the script-sides audit)", function()
        -- A script parked in `WaitForChild` and moved into storage: its old run
        -- must not come back when the child appears, beside nothing.
        local script = template("Waiter") :: Script
        script.Parent = folder()
        task.wait()
        script.Parent = game:GetService("ServerStorage")
        task.wait()
        local beacon = Instance.new("Folder")
        beacon.Name = "ScriptLiveBeacon"
        beacon.Parent = workspace
        task.wait()
        task.wait()
        expect(made()):ToBe("")
        beacon:Destroy()
        script:Destroy()
    end)

    it("stops even when it cleared its own script global (the script-sides audit)", function()
        local script = template("Escaper") :: Script
        script.Parent = folder()
        task.wait()
        task.wait()
        local whileIn = #tickFolder():GetChildren()
        expect(whileIn > 0):ToBe(true)
        script.Enabled = false
        task.wait()
        task.wait()
        task.wait()
        expect(#tickFolder():GetChildren()):ToBe(whileIn)
        script:Destroy()
    end)

    it("does not run in ReplicatedStorage or ServerStorage, and stops when moved there", function()
        -- Storage holds templates, and a template must not run itself (§3).
        local storage = game:GetService("ReplicatedStorage")
        local stored = maker("FromStorage")
        stored.Parent = storage
        task.wait()
        expect(made()):ToBe("")
        stored:Destroy()

        local script = template("Ticker") :: Script
        script.Parent = folder()
        task.wait()
        task.wait()
        local whileIn = #tickFolder():GetChildren()
        expect(whileIn > 0):ToBe(true)
        script.Parent = game:GetService("ServerStorage")
        task.wait()
        task.wait()
        expect(#tickFolder():GetChildren()):ToBe(whileIn)
        script:Destroy()
    end)
end)
