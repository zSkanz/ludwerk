--!strict
--- Conformance: a script carries the side it runs on (ADR 0138, S2).
---
--- `Script.RunContext` decides where a script outside the script services
--- runs: `Server` on the authority, `Client` where a player sits, `Shared` on
--- both. A conformance run is solo -- the server and a player at once -- so it
--- runs every side, and each script once.
---
--- The scripts' code comes from `content/stamps/spec-live.stamp.json`, where
--- the editor would have set each side, because a script writes neither
--- `Source` nor `RunContext`.

local testing = require("@engine/testing")

local describe, it, expect = testing.describe, testing.it, testing.expect
local beforeEach, afterEach = testing.beforeEach, testing.afterEach

local sandbox: Folder? = nil

local function folder(): Folder
    return assert(sandbox, "the sandbox folder is created in beforeEach")
end

local function template(name: string): Script
    return (Instance.stamp("spec-live"):FindFirstChild(name) :: Script):Clone() :: Script
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

describe("RunContext", function()
    beforeEach(function()
        local created = Instance.new("Folder")
        created.Name = "ScriptLiveSpec"
        created.Parent = workspace
        sandbox = created
    end)

    afterEach(function()
        folder():Destroy()
        sandbox = nil
    end)

    it("is Shared on a new script", function()
        local script = Instance.new("Script")
        expect(script.RunContext):ToBe(Enum.RunContext.Shared)
        script:Destroy()
    end)

    it("reads what the editor set, and a clone keeps it", function()
        expect(template("ServerMaker").RunContext):ToBe(Enum.RunContext.Server)
        expect(template("ClientMaker").RunContext):ToBe(Enum.RunContext.Client)
        expect((template("SharedMaker"):Clone() :: Script).RunContext):ToBe(Enum.RunContext.Shared)
    end)

    it("refuses a script's write", function()
        local script = Instance.new("Script")
        expect(function()
            (script :: any).RunContext = Enum.RunContext.Server
        end):ToThrow("a package fixes it")
        expect(script.RunContext):ToBe(Enum.RunContext.Shared)
        script:Destroy()
    end)

    it("runs every side on a solo machine, each script once", function()
        -- Solo is the server and a player at once (ADR 0138 §3): a `Shared`
        -- script that ran twice here would do everything twice in one world.
        template("ServerMaker").Parent = folder()
        template("ClientMaker").Parent = folder()
        template("SharedMaker").Parent = folder()
        task.wait()
        task.wait()
        expect(made()):ToBe("FromServer,FromClient,FromShared")
    end)
end)
