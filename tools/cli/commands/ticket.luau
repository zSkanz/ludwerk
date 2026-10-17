--!strict
--[[
	`ludwerk ticket keygen` and `ludwerk ticket sign` -- tickets for a game
	whose backend cannot sign them itself (ADR 0226 §3).

		ludwerk ticket keygen --out=ticket.key
		ludwerk ticket sign --key=ticket.key --sub=account-42 [--seconds=90] [path]

	`keygen` writes an Ed25519 private key as a PKCS #8 file -- the form any JWT
	library reads -- and prints the public half, which goes under `[network]
	ticket_public_key` in `project.toml`. `sign` prints a ticket for one
	account, for the project's `[project] id`, good for at most 90 seconds.

	**The engine does it**, with the code its servers check tickets with, so a
	ticket made here is one `VerifyTicket` reads exactly; this only asks.
]]

local path = require("@std/path")
local process = require("@std/process")

local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

export type Options = {
    action: string?,
    path: string,
    out: string?,
    key: string?,
    sub: string?,
    seconds: string?,
}

local function run(options: Options): number
    local engine = projectlib.findEngine()
    if engine == nil then
        print(tr("cli.err.no_engine"))
        return 3
    end
    if options.action == "keygen" then
        local out = path.format(path.resolve(options.out or "ticket.key"))
        local result = process.run({ engine, `--ticket-keygen={out}` }, { stdio = "inherit" })
        if not result.ok then
            return 1
        end
        print(tr("cli.ticket.keygen_done", { path = out }))
        return 0
    end
    if options.action == "sign" then
        if options.key == nil or options.sub == nil then
            print(tr("cli.ticket.err.usage"))
            return 2
        end
        local project = projectlib.load(options.path)
        if project.diagnostic ~= nil then
            print(project.diagnostic)
            return 2
        end
        local section = project.config.project
        local id = if typeof(section) == "table" then (section :: { [string]: any }).id else nil
        if typeof(id) ~= "string" or id == "" then
            print(tr("cli.ticket.err.no_id", { path = project.root }))
            return 2
        end
        local command = {
            engine,
            `--ticket-sign={path.format(path.resolve(options.key))}`,
            `--ticket-sub={options.sub}`,
            `--ticket-aud={id}`,
            `--ticket-seconds={options.seconds or "90"}`,
        }
        local result = process.run(command, { stdio = "inherit" })
        return if result.ok then 0 else 1
    end
    print(tr("cli.ticket.err.usage"))
    return 2
end

return {
    run = run,
}
