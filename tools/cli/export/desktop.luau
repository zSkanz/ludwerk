--!strict
--[[
	The Windows and Linux exports (ADR 0104 §7): a copy, a rename, a stamp and
	an archive, from any desktop.

		dist/windows/<Executable>.exe, content/, game/     and <name>.zip beside it
		dist/linux/<executable>, content/, game/,
		           <executable>.desktop, icon.png          and <name>.tar.gz beside it

	**The archive is written by `assetc archive`, not by a system tool**,
	because a `.tar.gz` made on Windows has to carry the execute bit a Linux
	player needs and no Windows archiver writes one. `assetc` is the one binary
	every installation already has beside the CLI.
]]

local fs = require("@std/fs")
local process = require("@std/process")

local catalog = require("../catalog")
local projectlib = require("../project")
local targets = require("./targets")

local tr = catalog.tr

export type Identity = {
    -- `[project] name`, for a person: the window, the launcher, the file properties.
    displayName: string,
    -- The same, safe as a file name.
    fileName: string,
    version: string?,
    company: string?,
    -- The project's icon, resolved, when it names one that exists.
    icon: string?,
}

--- `[export.<platform>] executable`, or the name. A Linux one is lower case by
--- convention, and a Windows one keeps the author's spelling.
local function executableName(project: projectlib.Project, identity: Identity, platformName: string): string
    local export = project.config.export
    local section = if typeof(export) == "table" then export[platformName] else nil
    local configured = if typeof(section) == "table" and typeof(section.executable) == "string"
        then section.executable
        else nil
    local base = configured or identity.fileName
    base = string.gsub(base, "[^%w_%-%.]", "-")
    if base == "" then
        base = "game"
    end
    if platformName == "linux" and configured == nil then
        base = string.lower(base)
    end
    return if platformName == "windows" then `{base}.exe` else base
end

--- The icon files a target needs, made from the project's one picture into
--- `stage` (ADR 0104 §2). Returns the `.ico` for Windows or the 512 px PNG for
--- Linux; nil when the project names no icon, or names an `.ico` and the target
--- is not Windows.
local function makeIcon(identity: Identity, platformName: string, stage: string): (string?, string?)
    local icon = identity.icon
    if icon == nil then
        return nil, nil
    end
    local extension = string.lower(string.match(icon, "%.(%w+)$") or "")
    if extension == "ico" then
        -- v1's spelling, which still works for Windows, and only for it.
        return if platformName == "windows" then icon else nil, nil
    end
    local assetc = projectlib.findTool("assetc")
    if assetc == nil then
        return nil, tr("cli.assets.err.no_compiler")
    end
    local made = process.run({ assetc, "icon", icon, "--out", stage, "--targets", platformName })
    if not made.ok then
        return nil, made.stdout .. made.stderr
    end
    local file = if platformName == "windows"
        then projectlib.joined(stage, "windows", "icon.ico")
        else projectlib.joined(stage, "linux", "icon.png")
    return if fs.exists(file) then file else nil, nil
end

--- The icon and the file properties, into the copied executable, then read
--- back out of it (ADR 0104 §7). Windows only: `iconpatch` is `UpdateResourceW`.
local function stampWindows(executable: string, ico: string?, identity: Identity): string?
    local tool = projectlib.findTool("iconpatch")
    if tool == nil then
        print(tr("cli.build.warn.no_iconpatch"))
        return nil
    end
    if ico ~= nil then
        local patched = process.run({ tool, "replace", executable, ico })
        if not patched.ok then
            return patched.stdout .. patched.stderr .. "\n" .. tr("cli.build.err.icon_failed", { path = ico })
        end
        -- **Against the engine's own mark**, when this installation has it: a
        -- patch that left the default in place is a verify that passes on some
        -- icon and fails on the one that matters.
        local command = { tool, "verify", executable }
        local root = projectlib.installRoot()
        local engineIcon = if root ~= nil then projectlib.joined(root, "branding", "icon", "icon.ico") else nil
        if engineIcon ~= nil and fs.exists(engineIcon) then
            table.insert(command, "--not")
            table.insert(command, engineIcon)
        end
        local verified = process.run(command)
        if not verified.ok then
            return verified.stdout .. verified.stderr .. "\n" .. tr("cli.build.err.icon_failed", { path = ico })
        end
    end
    local command = {
        tool,
        "version",
        executable,
        "--product",
        identity.displayName,
        "--description",
        identity.displayName,
        "--version",
        identity.version or "0.0.0",
    }
    if identity.company ~= nil then
        table.insert(command, "--company")
        table.insert(command, identity.company)
    end
    local versioned = process.run(command)
    if not versioned.ok then
        return versioned.stdout .. versioned.stderr .. "\n" .. tr("cli.build.err.version_failed")
    end
    return nil
end

--- A Linux launcher entry. `Exec` and `Icon` are relative, which a desktop
--- does not resolve on its own: it is the file an installer, or a person
--- copying it to `~/.local/share/applications/`, completes with the folder.
local function desktopEntry(identity: Identity, executable: string): string
    local lines = {
        "[Desktop Entry]",
        "Type=Application",
        `Name={identity.displayName}`,
        `Exec={executable}`,
        "Icon=icon",
        "Terminal=false",
        "Categories=Game;",
    }
    if identity.version ~= nil then
        table.insert(lines, `X-AppVersion={identity.version}`)
    end
    if identity.company ~= nil then
        table.insert(lines, `X-Company={identity.company}`)
    end
    return table.concat(lines, "\n") .. "\n"
end

--- The archive's name beside the folder: `<name>-<version>-<target>`.
local function archiveBase(identity: Identity, target: targets.Target): string
    local version = if identity.version ~= nil then `-{identity.version}` else ""
    return `{identity.fileName}{version}-{target.name}`
end

--- `assetc archive`, over the folder, into `archive`. `executables` are paths
--- inside the folder that carry mode 0755.
local function writeArchive(folder: string, archive: string, prefix: string, executables: { string }): string?
    local assetc = projectlib.findTool("assetc")
    if assetc == nil then
        return tr("cli.assets.err.no_compiler")
    end
    local command = { assetc, "archive", folder, "--out", archive, "--prefix", prefix }
    for _, executable in executables do
        table.insert(command, "--executable")
        table.insert(command, executable)
    end
    local written = process.run(command)
    if not written.ok then
        return written.stdout .. written.stderr .. "\n" .. tr("cli.build.err.archive_failed", { path = archive })
    end
    return nil
end

return {
    executableName = executableName,
    makeIcon = makeIcon,
    stampWindows = stampWindows,
    desktopEntry = desktopEntry,
    archiveBase = archiveBase,
    writeArchive = writeArchive,
}
