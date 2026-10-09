--!strict
--[[
	`ludwerk check` and `ludwerk fmt`: what they walk and how they name what
	they load.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local test = require("@std/test")

local check = require("../commands/check")

local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    -- Beside the tests rather than under `.lute/`, which is itself ignored.
    local root = `{base}/scratch-check-{name}`
    if fs.exists(root) then
        fs.removeDirectory(root, { recursive = true })
    end
    fs.createDirectory(root, { makeParents = true })
    return root
end

test.suite("check", function(suite)
    suite:case("large analysis commands keep definitions and every file within the spawn budget", function()
        local prefix = { "luau-lsp", "analyze", "--definitions=@engine=types.d.luau" }
        local files: { string } = {}
        for index = 1, 839 do
            table.insert(files, `C:/Users/developer/Documents/A Game With Spaces/src/Module{index}.luau`)
        end
        local commands = check.analysisCommands(prefix, files)
        assert.eq(#commands > 1, true)
        local seen = 0
        for _, command in commands do
            local size = 0
            for _, arg in command do
                size += #arg + 3
            end
            assert.eq(size <= 24000, true)
            for index, arg in prefix do
                assert.eq(command[index], arg)
            end
            for index = #prefix + 1, #command do
                seen += 1
                assert.eq(command[index], files[seen])
            end
        end
        assert.eq(seen, #files)
        assert.eq(#check.analysisCommands(prefix, {}), 0)
    end)

    suite:case("an exported game and the engine's own folder are never walked", function()
        -- **D211.** `ludwerk build` writes `dist/`, and check and fmt went
        -- through every exported copy of the game with the source.
        local root = scratch("walk")
        for _, folder in { "src", "dist/windows/content/src", ".engine/types", "src/distance" } do
            fs.createDirectory(`{root}/{folder}`, { makeParents = true })
        end
        fs.writeStringToFile(`{root}/src/main.luau`, "return nil\n")
        fs.writeStringToFile(`{root}/src/distance/near.luau`, "return nil\n")
        fs.writeStringToFile(`{root}/dist/windows/content/src/main.luau`, "return nil\n")
        fs.writeStringToFile(`{root}/.engine/types/stale.luau`, "return nil\n")

        local files = check.collectLuau(root, { "third_party", ".lute", "luau_packages", ".engine", "dist" })
        local names: { string } = {}
        for _, file in files do
            table.insert(names, (string.gsub(string.sub(file, #root + 2), "\\", "/")))
        end
        table.sort(names)
        -- `distance/` is a folder that only starts like one ignored.
        assert.eq(table.concat(names, ","), "src/distance/near.luau,src/main.luau")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a game's catalogs are checked against its default locale's", function()
        -- ADR 0154: a key the default locale has and another lacks is a
        -- player reading another language's text.
        local root = scratch("catalogs")
        fs.createDirectory(`{root}/i18n`, { makeParents = true })
        fs.writeStringToFile(`{root}/i18n/en.json`, '{"menu.play": "Play", "menu.quit": "Quit", "hud.score": "Score"}')
        fs.writeStringToFile(`{root}/i18n/pt-BR.json`, '{"menu.play": "Jogar", "hud.vidas": "Vidas"}')
        fs.writeStringToFile(
            `{root}/i18n/es.json`,
            '{"menu.play": "Jugar", "menu.quit": "Salir", "hud.score": "Puntos"}'
        )

        local report = check.checkCatalogs(root)
        assert.eq(report.catalogs, 3)
        assert.eq(#report.unreadable, 0)
        -- By locale, then by key: the same list on every machine.
        assert.eq(#report.missing, 2)
        assert.eq(`{report.missing[1].locale}:{report.missing[1].key}`, "pt-BR:hud.score")
        assert.eq(`{report.missing[2].locale}:{report.missing[2].key}`, "pt-BR:menu.quit")
        assert.eq(#report.extra, 1)
        assert.eq(`{report.extra[1].locale}:{report.extra[1].key}`, "pt-BR:hud.vidas")

        -- The default is the project's to name.
        fs.writeStringToFile(`{root}/project.toml`, '[project]\nname = "x"\ndefault_locale = "es"\n')
        local other = check.checkCatalogs(root)
        assert.eq(#other.missing, 2)
        assert.eq(other.missing[1].locale, "pt-BR")
        assert.eq(#other.extra, 1)

        -- A file that is not a catalog is said, not skipped.
        fs.writeStringToFile(`{root}/i18n/fr.json`, "not json")
        assert.eq(check.checkCatalogs(root).unreadable[1], "i18n/fr.json")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("an engine key a game re-words in one locale is not a stray key", function()
        -- Found by `examples/33-options`: its pt-BR catalog says the options
        -- screen's buttons, and the lint called each a key nothing asks for.
        local root = scratch("enginekeys")
        fs.createDirectory(`{root}/i18n`, { makeParents = true })
        fs.writeStringToFile(`{root}/i18n/en.json`, '{"menu.play": "Play"}')
        fs.writeStringToFile(
            `{root}/i18n/pt-BR.json`,
            '{"menu.play": "Jogar", "engine.settings.apply": "Aplicar", "scene.err.thing": "x", "hud.vidas": "Vidas"}'
        )

        -- With the engine's catalog to hand, its keys are known by name.
        local known = check.checkCatalogs(root, { ["engine.settings.apply"] = true, ["scene.err.thing"] = true })
        assert.eq(#known.missing, 0)
        assert.eq(#known.extra, 1)
        assert.eq(known.extra[1].key, "hud.vidas")

        -- Without it, what is under `engine.` is taken for the engine's.
        local guessed = check.checkCatalogs(root)
        assert.eq(#guessed.extra, 2)
        assert.eq(guessed.extra[1].key, "hud.vidas")
        assert.eq(guessed.extra[2].key, "scene.err.thing")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a project with no catalogs, or one, has nothing to compare", function()
        local root = scratch("nocatalogs")
        assert.eq(check.checkCatalogs(root).catalogs, 0)
        fs.createDirectory(`{root}/i18n`, { makeParents = true })
        fs.writeStringToFile(`{root}/i18n/en.json`, '{"menu.play": "Play"}')
        local report = check.checkCatalogs(root)
        assert.eq(report.catalogs, 1)
        assert.eq(#report.missing, 0)
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("stylua is told to skip what check skips", function()
        local command = check.styluaCommand({ "stylua", "--check" }, ".", { ".engine", "dist" })
        local joined = table.concat(command, " ")
        assert.strContains(joined, "--glob=!**/dist/**")
        assert.strContains(joined, "--glob=!**/.engine/**")
        assert.strContains(joined, "--glob=!**/*.d.luau")
        assert.eq(command[#command], ".")
    end)

    suite:case("every scene of a project is a tree to check, in a fixed order", function()
        -- A script is mounted by the scene it belongs to (ADR 0105), so the
        -- editor's checker is run over each.
        local root = scratch("scenes")
        assert.eq(#check.projectScenes(root), 0)
        fs.createDirectory(`{root}/content/scenes/levels`, { makeParents = true })
        fs.writeStringToFile(`{root}/content/scenes/menu.scene.json`, "{}")
        fs.writeStringToFile(`{root}/content/scenes/levels/one.scene.json`, "{}")
        fs.writeStringToFile(`{root}/content/scenes/notes.json`, "{}")
        local scenes = check.projectScenes(root)
        assert.eq(table.concat(scenes, " "), "scenes/levels/one.scene.json scenes/menu.scene.json")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a project is checked as the editor checks it, where an engine with a checker is", function()
        -- **The owner's report** (D583): a game `check` called clean, 193
        -- errors in the editor. The command asks the editor's checker now.
        local projectlib = require("../project")
        local engine = projectlib.findEngine()
        if engine == nil then
            print("no engine is built here, so the editor's checker was not asked")
            return
        end
        local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
        local project = `{base}/../../../tests/scriptcheck/project`
        local clean = check.editorCheck(engine :: string, project)
        if not clean.ran then
            print("the engine found here has no script checker, so it was not asked")
            return
        end
        assert.eq(clean.failure, nil)
        assert.eq(#clean.problems, 0)

        -- And a mistake is said, once, with its line.
        local root = scratch("editor")
        for _, folder in { "src/shared", "src/client" } do
            fs.createDirectory(`{root}/{folder}`, { makeParents = true })
        end
        fs.writeStringToFile(`{root}/project.toml`, '[project]\nname = "Broken"\nid = "dev.ludwerk.broken"\n')
        fs.writeStringToFile(
            `{root}/src/shared/Match.luau`,
            "--!strict\nexport type Context = { Round: number }\nreturn { make = function(): Context\n    return { Round = 1 }\nend }\n"
        )
        fs.writeStringToFile(
            `{root}/src/client/Main.luau`,
            '--!strict\nlocal Match = require("../shared/Match")\nprint(Match.make().Rounds)\n'
        )
        local broken = check.editorCheck(engine :: string, root)
        assert.eq(broken.errors, 1)
        assert.eq(#broken.problems, 1)
        assert.strContains(broken.problems[1].line, "src/client/Main.luau(3,")
        assert.strContains(broken.problems[1].line, "Rounds")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("definitions load under the engine's own name", function()
        -- **D212.** Unnamed, the analyser announced them under another
        -- platform's.
        assert.eq(check.definitionName("runtime/types/engine.d.luau"), "engine")
        assert.eq(check.definitionName("C:\\game\\.engine\\types\\scene.d.luau"), "scene")
    end)
end)
