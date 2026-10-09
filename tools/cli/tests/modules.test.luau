--!strict
local assert = require("@std/test/assert")
local exporter = require("../modules/export")
local fs = require("@std/fs")
local game = require("../export/game")
local manager = require("../modules/manager")
local projectlib = require("../project")
local registry = require("../modules/registry")
local targets = require("../export/targets")
local test = require("@std/test")

test.suite("optional modules", function(suite)
    suite:case("project enablement is independent of SDK installation", function()
        local rows = manager.rows({ integrations = { enabled = { "xbox" } } })
        local found = false
        for _, row in rows do
            if row.integration == "xbox" then
                found = true
                assert.eq(row.enabled, true)
            end
        end
        assert.eq(found, true)
        assert.eq(manager.enabled({}, "xbox"), false)
        assert.eq(manager.enabled({ integrations = { enabled = {} } }, "xbox"), false)
    end)
    suite:case("unknown dependencies fail before any installer runs", function()
        local ordered, failure = registry.resolve("missing-module")
        assert.eq(ordered, nil)
        assert.eq(failure, "UnknownModule")
        local gdk = registry.find("microsoft-gdk")
        assert.that(gdk ~= nil)
        local resolved, error = registry.resolve("microsoft-gdk")
        assert.eq(error, nil)
        assert.that(resolved ~= nil)
        assert.eq(#(resolved :: { registry.Module }), 1)
    end)
    suite:case("disabled and unsupported integrations carry no native payload", function()
        local windows = targets.find("windows") :: targets.Target
        local linux = targets.find("linux") :: targets.Target
        local project = projectlib.load(".")
        project.config = {}
        local payloads, error = exporter.prepare(project, windows, "game.exe")
        assert.eq(error, nil)
        assert.eq(#(payloads :: { exporter.Payload }), 0)
        project.config = { integrations = { enabled = { "xbox" } } }
        payloads, error = exporter.prepare(project, linux, "game")
        assert.eq(error, nil)
        assert.eq(#(payloads :: { exporter.Payload }), 0)
    end)
    suite:case("deployment copies only declared runtime files", function()
        local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
        local root = `{base}/scratch-optional-modules`
        if fs.exists(root) then
            game.removeTree(root)
        end
        fs.createDirectory(`{root}/provider`, { makeParents = true })
        fs.writeStringToFile(`{root}/provider/native.dll`, "runtime")
        fs.writeStringToFile(`{root}/provider/private.h`, "must not ship")
        local item: registry.Module = {
            Id = "test",
            Label = "test",
            Kind = "integration",
            Targets = {},
            Dependencies = {},
            Integration = "test",
            NativeFiles = { "native.dll" },
        }
        exporter.deploy({ { Item = item, Directory = `{root}/provider`, GameConfig = nil } }, `{root}/output`)
        assert.eq(fs.readFileToString(`{root}/output/integrations/test/native.dll`), "runtime")
        assert.eq(fs.exists(`{root}/output/integrations/test/private.h`), false)
        game.removeTree(root)
    end)
end)
