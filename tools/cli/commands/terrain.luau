--!strict
--[[
	`ludwerk terrain import <source> [path]` -- a project's terrain, laid from a
	source and saved (ADR 0149 §2).

		ludwerk terrain import island.png --size=4096 --low=-20 --high=380
		ludwerk terrain import tiles/     --size=16384 --low=0 --high=900
		ludwerk terrain import world.luau --size=16384
		ludwerk terrain import hills      --size=8192 --high=160 --scale=900 --seed=7

	`<source>` is a heightmap -- a sixteen-bit PNG, a RAW of sixteen-bit
	samples, or a folder of tiles of either named `<anything>_x<column>_y<row>`
	-- a Luau file that returns `function(x, z)` answering a height, or the
	word `hills` for hills from noise. `--size` is metres across; `--low` and
	`--high` are black and white for a heightmap, and the hills' floor and top.

	**The engine does it, and this only asks**: the scene is what the editor
	holds, the verbs are the editor's own, and the save is the editor's save --
	so a world made here is the one its Create tab would have made. What this
	adds is that it can be a build step, and that nothing has to be open.

	**It is laid a tile at a time** where it is larger than one table, with the
	ground behind it written out as it goes: a world sixteen kilometres across
	at a metre a voxel is made in the memory a tile takes.
]]

local fs = require("@std/fs")
local path = require("@std/path")
local process = require("@std/process")

local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

export type Options = {
    action: string?,
    source: string?,
    path: string,
    size: string?,
    low: string?,
    high: string?,
    material: string?,
    seed: string?,
    scale: string?,
    octaves: string?,
}

-- Each of an import's numbers, as the engine names it.
local Numbers: { { option: string, flag: string } } = {
    { option = "size", flag = "--import-size" },
    { option = "low", flag = "--import-low" },
    { option = "high", flag = "--import-high" },
    { option = "material", flag = "--import-material" },
    { option = "seed", flag = "--import-seed" },
    { option = "scale", flag = "--import-scale" },
    { option = "octaves", flag = "--import-octaves" },
}

--- The engine's arguments for `options`, or nothing and why. Apart from `run`
--- so it can be tested without an engine.
local function arguments(options: Options): ({ string }?, string?)
    if options.action ~= "import" or options.source == nil then
        return nil, tr("cli.terrain.err.usage")
    end
    local source = options.source
    if source ~= "hills" then
        -- Whole, because the engine runs in the project and not here.
        source = path.format(path.resolve(source))
        if not fs.exists(source) then
            return nil, tr("cli.terrain.err.no_source", { source = source })
        end
    end
    if options.size == nil then
        return nil, tr("cli.terrain.err.no_size")
    end
    local out = { `--import-terrain={source}` }
    local values = options :: { [string]: string? }
    for _, number in Numbers do
        local value = values[number.option]
        if value ~= nil then
            if tonumber(value) == nil then
                return nil, tr("cli.terrain.err.not_a_number", { option = number.option, value = value })
            end
            table.insert(out, `{number.flag}={value}`)
        end
    end
    return out, nil
end

local function run(options: Options): number
    local engineArguments, refusal = arguments(options)
    if engineArguments == nil then
        print(refusal)
        return 2
    end
    local project = projectlib.load(options.path)
    if project.diagnostic ~= nil then
        print(project.diagnostic)
        return 2
    end
    if not projectlib.isProject(project) then
        print(tr("cli.err.not_a_project", { path = project.root }))
        return 2
    end
    -- 3, as `edit` answers a missing engine: the environment is wrong, not
    -- the command line.
    local engine = projectlib.findEngine()
    if engine == nil then
        print(tr("cli.err.no_engine"))
        return 3
    end
    local command = { engine, project.root }
    table.move(engineArguments, 1, #engineArguments, #command + 1, command)
    -- Its own lines are the progress: nothing is captured.
    local result = process.run(command, { stdio = "inherit" })
    return if result.ok then 0 else 1
end

return {
    run = run,
    arguments = arguments,
}
