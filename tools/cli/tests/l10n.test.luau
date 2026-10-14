--!strict
--[[
	The voices an export ships apart (ADR 0200 §7): which languages a project
	has, which `[export] voice` keeps in the game's own pack, and what is taken
	out of the staged content for the others.
]]

local assert = require("@std/test/assert")
local fs = require("@std/fs")
local test = require("@std/test")

local l10n = require("../export/l10n")
local projectlib = require("../project")
local toml = require("../toml")

local function scratch(name: string): string
    local base = string.match(debug.info(1, "s"), "^(.*)[/\\][^/\\]+$") or "tools/cli/tests"
    local root = `{base}/scratch-l10n-{name}`
    if fs.exists(root) then
        fs.removeDirectory(root, { recursive = true })
    end
    fs.createDirectory(root, { makeParents = true })
    return root
end

local function write(root: string, file: string, text: string)
    local folder = string.match(file, "^(.*)/[^/]+$")
    if folder ~= nil then
        fs.createDirectory(`{root}/{folder}`, { makeParents = true })
    end
    fs.writeStringToFile(`{root}/{file}`, text)
end

--- A content directory with English as its plain files, two voice languages
--- and one that only has a picture.
local function content(name: string): string
    local root = scratch(name)
    write(root, "voice/intro_01.ogg", "english")
    write(root, "l10n/pt-BR/voice/intro_01.ogg", "portuguese")
    write(root, "l10n/pt-BR/voice/deep/intro_02.WAV", "portuguese")
    write(root, "l10n/pt-BR/ui/logo.png", "a picture")
    write(root, "l10n/ja/voice/intro_01.ogg", "japanese")
    write(root, "l10n/de/ui/logo.png", "a picture")
    return root
end

local function projectWith(config: string): projectlib.Project
    local parsed = toml.parse(config)
    assert.eq(parsed.ok, true)
    return { root = ".", config = parsed.table }
end

test.suite("l10n", function(suite)
    suite:case("a voice is told by its ending, and a voice language by holding one", function()
        assert.eq(l10n.isSound("intro_01.ogg"), true)
        assert.eq(l10n.isSound("intro_01.WAV"), true)
        assert.eq(l10n.isSound("a.Mp3"), true)
        assert.eq(l10n.isSound("a.flac"), true)
        assert.eq(l10n.isSound("logo.png"), false)
        assert.eq(l10n.isSound(".ogg"), false)

        local root = content("languages")
        assert.eq(table.concat(l10n.folders(root), ","), "de,ja,pt-BR")
        -- A language with pictures and no sound is not a voice language.
        assert.eq(table.concat(l10n.voiceLanguages(root), ","), "ja,pt-BR")
        assert.eq(
            table.concat(l10n.soundsUnder(`{root}/l10n/pt-BR`), ","),
            "voice/deep/intro_02.WAV,voice/intro_01.ogg"
        )
        assert.eq(l10n.hasAnything(root), true)

        -- A project with no language and no lines file has nothing to index.
        local bare = scratch("bare")
        write(bare, "scenes/main.scene.json", "{}")
        write(bare, "dialogue/notes.json", "{}")
        assert.eq(l10n.hasAnything(bare), false)
        -- Lines alone are something.
        write(bare, "dialogue/act1.lines.json", "{}")
        assert.eq(l10n.hasAnything(bare), true)
        fs.removeDirectory(root, { recursive = true })
        fs.removeDirectory(bare, { recursive = true })
    end)

    suite:case("[export] voice names what ships in the game's pack; absent, every language does", function()
        local root = content("plan")

        local all = l10n.plan(projectWith('[project]\nname = "x"\n'), root, "en", false)
        assert.that(all ~= nil, "a project that says nothing ships every language")
        local everything = all :: l10n.Plan
        assert.eq(table.concat(everything.voice, ","), "ja,pt-BR")
        assert.eq(table.concat(everything.shipped, ","), "ja,pt-BR")
        assert.eq(#everything.apart, 0)

        -- The default language always ships -- it is the plain files -- and
        -- naming it is not a mistake.
        local some = l10n.plan(projectWith('[export]\nvoice = ["en", "pt-BR"]\n'), root, "en", false) :: l10n.Plan
        assert.eq(table.concat(some.voice, ","), "ja,pt-BR")
        assert.eq(table.concat(some.shipped, ","), "pt-BR")
        assert.eq(table.concat(some.apart, ","), "ja")
        assert.eq(#some.overruled, 0)

        -- None named: every language ships apart.
        local none = l10n.plan(projectWith("[export]\nvoice = []\n"), root, "en", false) :: l10n.Plan
        assert.eq(#none.shipped, 0)
        assert.eq(table.concat(none.apart, ","), "ja,pt-BR")

        -- A folder with no sound may be named: it is a language of the game.
        local pictures = l10n.plan(projectWith('[export]\nvoice = ["de"]\n'), root, "en", false) :: l10n.Plan
        assert.eq(table.concat(pictures.apart, ","), "ja,pt-BR")

        -- A name that is neither a folder nor the default is said, by name.
        local unknown, why = l10n.plan(projectWith('[export]\nvoice = ["en", "pt-PT"]\n'), root, "en", false)
        assert.eq(unknown, nil)
        assert.strContains(why :: string, "pt-PT")
        -- And `en` is only the default while the project says so.
        local other, otherWhy = l10n.plan(projectWith('[export]\nvoice = ["en"]\n'), root, "fr", false)
        assert.eq(other, nil)
        assert.strContains(otherWhy :: string, '"en"')
        -- Not a list.
        local wrong = l10n.plan(projectWith('[export]\nvoice = "pt-BR"\n'), root, "en", false)
        assert.eq(wrong, nil)

        -- A phone has no language packs: every language ships, and what the
        -- list would have left out is what the build says.
        local phone = l10n.plan(projectWith('[export]\nvoice = ["pt-BR"]\n'), root, "en", true) :: l10n.Plan
        assert.eq(table.concat(phone.shipped, ","), "ja,pt-BR")
        assert.eq(#phone.apart, 0)
        assert.eq(table.concat(phone.overruled, ","), "ja")
        fs.removeDirectory(root, { recursive = true })
    end)

    suite:case("a language's sounds leave the staged content at the paths they had; its pictures stay", function()
        local staged = content("staged")
        local apart = scratch("apart")
        assert.eq(l10n.takeApart(staged, "pt-BR", apart), 2)

        assert.eq(fs.readFileToString(`{apart}/l10n/pt-BR/voice/intro_01.ogg`), "portuguese")
        assert.eq(fs.exists(`{apart}/l10n/pt-BR/voice/deep/intro_02.WAV`), true)
        assert.eq(fs.exists(`{staged}/l10n/pt-BR/voice/intro_01.ogg`), false)
        assert.eq(fs.exists(`{staged}/l10n/pt-BR/voice/deep/intro_02.WAV`), false)
        -- The picture, the other language and the default are untouched.
        assert.eq(fs.exists(`{staged}/l10n/pt-BR/ui/logo.png`), true)
        assert.eq(fs.exists(`{apart}/l10n/pt-BR/ui/logo.png`), false)
        assert.eq(fs.exists(`{staged}/l10n/ja/voice/intro_01.ogg`), true)
        assert.eq(fs.exists(`{staged}/voice/intro_01.ogg`), true)
        fs.removeDirectory(staged, { recursive = true })
        fs.removeDirectory(apart, { recursive = true })
    end)

    suite:case("the packs beside an export are this build's, and nothing else there is touched", function()
        local built = scratch("built")
        local folder = scratch("languages")
        write(built, "l10n-ja.lpack", "ja pack")
        write(built, "l10n-ja.manifest.json", "ja names")
        -- Last build's: a language that ships inside the game now, and the
        -- same one from before the game was sealed. And a file of somebody's.
        write(folder, "l10n-pt-BR.lpack", "stale")
        write(folder, "l10n-ja.manifest.json", "stale")
        write(folder, "notes.txt", "mine")

        assert.eq(table.concat(l10n.packFiles("ja", true), ","), "l10n-ja.lpack")
        assert.eq(table.concat(l10n.packFiles("ja", false), ","), "l10n-ja.lpack,l10n-ja.manifest.json")

        l10n.publish(built, folder, { "ja" }, true)
        assert.eq(fs.readFileToString(`{folder}/l10n-ja.lpack`), "ja pack")
        assert.eq(fs.exists(`{folder}/l10n-pt-BR.lpack`), false)
        assert.eq(fs.exists(`{folder}/l10n-ja.manifest.json`), false)
        assert.eq(fs.readFileToString(`{folder}/notes.txt`), "mine")

        -- Not sealed, a language is its pack and its manifest.
        l10n.publish(built, folder, { "ja" }, false)
        assert.eq(fs.readFileToString(`{folder}/l10n-ja.manifest.json`), "ja names")

        -- No language apart any more: the packs go, the folder's own stay.
        l10n.publish(folder, folder, {}, true)
        assert.eq(fs.exists(`{folder}/l10n-ja.lpack`), false)
        assert.eq(fs.exists(`{folder}/l10n-ja.manifest.json`), false)
        assert.eq(fs.exists(`{folder}/notes.txt`), true)
        fs.removeDirectory(built, { recursive = true })
        fs.removeDirectory(folder, { recursive = true })
    end)
end)
