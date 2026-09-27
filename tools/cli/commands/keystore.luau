--!strict
--[[
	`ludwerk keystore new` — the key an Android game is released with (ADR 0104
	§3), and what the editor's **Create keystore...** button runs.

	**The password never goes on a command line**, where every other process on
	the machine can read it: it comes from `ENG_ANDROID_KEYSTORE_PASSWORD`
	(and `ENG_ANDROID_KEY_PASSWORD`, when the key's differs), which is how the
	editor hands it over and how a CI job already has it. `--remember` keeps it
	in the per-user store beside the editor's preferences, outside every project,
	so the next release export finds it without being told.

	It says once, plainly, what losing the file means: a store accepts an update
	only when it is signed with the same key.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local process = require("@std/process")

local Brand = require("../brand")
local android = require("../export/android")
local catalog = require("../catalog")

local tr = catalog.tr

export type Options = {
    alias: string?,
    out: string?,
    -- The certificate's common name: who made the game.
    name: string?,
    -- In years.
    validity: number?,
    remember: boolean,
}

local function run(options: Options): number
    if options.alias == nil or options.out == nil then
        print(tr("cli.keystore.err.usage"))
        return 2
    end
    local alias = options.alias :: string
    local out = path.format(path.resolve(options.out :: string))
    if fs.exists(out) then
        -- Refused, never replaced: a keystore overwritten is every published
        -- update lost.
        print(tr("cli.keystore.err.exists", { path = out }))
        return 1
    end
    local password = process.env.ENG_ANDROID_KEYSTORE_PASSWORD
    if password == nil or #password < 6 then
        print(tr("cli.keystore.err.no_password"))
        return 2
    end
    local keyPassword = process.env.ENG_ANDROID_KEY_PASSWORD
    if keyPassword == nil or keyPassword == "" then
        keyPassword = password
    end

    local tools, toolsError = android.findTools()
    if tools == nil then
        print(toolsError)
        return 1
    end
    local parent = path.dirname(out)
    if parent ~= "" then
        fs.createDirectory(parent, { makeParents = true })
    end
    local name = options.name or (Brand.read().Name .. " Developer")
    local years = options.validity or 25
    local made = android.newKeystore(tools :: android.Tools, {
        keystore = out,
        alias = alias,
        storePassword = password :: string,
        keyPassword = keyPassword :: string,
        commonName = name,
        days = math.floor(years * 365),
    })
    if made ~= nil then
        print(made)
        return 1
    end
    if options.remember then
        android.rememberPasswords(out, password :: string, keyPassword :: string)
        print(tr("cli.keystore.remembered"))
    end
    print(tr("cli.keystore.created", { path = out, alias = alias }))
    print(tr("cli.keystore.warn.keep_it"))
    return 0
end

--- `ludwerk keystore remember --out <file>`: keeps an existing keystore's
--- passwords, from the same two variables, in the per-user store -- what the
--- Export window's "remember on this machine" does once a release export has
--- proved the password opens the key.
local function remember(out: string?): number
    local password = process.env.ENG_ANDROID_KEYSTORE_PASSWORD
    if out == nil or password == nil or password == "" then
        print(tr("cli.keystore.err.usage"))
        return 2
    end
    local keystore = path.format(path.resolve(out :: string))
    if not fs.exists(keystore) then
        print(tr("cli.build.err.android_no_keystore", { path = keystore }))
        return 1
    end
    local keyPassword = process.env.ENG_ANDROID_KEY_PASSWORD
    android.rememberPasswords(
        keystore,
        password :: string,
        if keyPassword ~= nil and keyPassword ~= "" then keyPassword else password :: string
    )
    print(tr("cli.keystore.remembered"))
    return 0
end

return {
    run = run,
    remember = remember,
}
