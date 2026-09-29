--!strict
--[[
	`ludwerk android install-tools` — what an Android export needs on a machine
	that has never built for Android (ADR 0104 §6): a JDK 17, the SDK's command
	line tools, platform 35, build-tools 35.0.0 and platform-tools (adb), and
	the Gradle the template pins. **No NDK**: the player is prebuilt.

	Everything goes into the user's Ludwerk folder -- never on PATH, never
	JAVA_HOME -- so a machine's own Java is left alone (Gradle 8.12 does not run
	on 24+). It is idempotent, and it checks by version: a piece already there
	at the pinned version is skipped, and a second run installs only what is
	missing.

	Downloads go through `curl` and archives open with `tar`, which Windows 10
	and later ship (its `tar` is bsdtar and reads a `.zip`); on Linux a `.zip`
	opens with `unzip`, or with the JDK's own `jar` when there is no `unzip`.

	Installing the SDK packages accepts their licences on the user's behalf,
	which the Export window's **Install** button says before it runs this.
]]

local crypto = require("@lute/crypto")
local fs = require("@std/fs")
local platform = require("@std/system/platform")
local process = require("@std/process")

local android = require("../export/android")
local catalog = require("../catalog")
local projectlib = require("../project")

local tr = catalog.tr

-- The pins (R5). The JDK is Temurin's exact build; the command-line tools are
-- the build `scripts/install-android.ps1` installs; Gradle is the template's.
local JdkVersion = "17.0.20.1+1"
local CmdlineToolsBuild = "13114758"
local GradleVersion = "8.12"
local SdkPackages = { "platforms;android-35", "build-tools;35.0.0", "platform-tools" }

-- **What each pinned archive's bytes are** (audit T2), SHA-256: the JDK's as
-- Adoptium publishes it, Gradle's as Gradle does, and the command-line tools'
-- as computed from the archives whose SHA-1 Google's repository lists. A
-- download that is not these bytes is not installed, and neither is a cached
-- one -- a truncated or replaced archive was trusted for ever.
local Sha256: { [string]: string } = {
    ["jdk-windows"] = "e53a79c3c3d86865bd7e787903884331068e71321714ffd44f145785affc7cb0",
    ["jdk-linux"] = "3808d1d15e3ec6bd5b84057fb5d84c33d8a1536a258146bcea2e603fc726e08e",
    ["gradle"] = "7a00d51fb93147819aab76024feece20b6b84e420694101f276be952e08bef03",
    ["cmdline-tools-windows"] = "98b565cb657b012dae6794cefc0f66ae1efb4690c699b78a614b4a6a3505b003",
    ["cmdline-tools-linux"] = "7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee",
}

local function exe(name: string): string
    return if platform.win32 then `{name}.exe` else name
end

local function osName(): string
    return if platform.win32 then "windows" else "linux"
end

local function step(message: string)
    print(tr("cli.android.step", { step = message }))
end

--- A file's SHA-256, as lowercase hex.
local function sha256Of(file: string): string
    local digest = crypto.digest(crypto.hash.sha256, fs.readFileToString(file))
    local hex = table.create(buffer.len(digest))
    for at = 0, buffer.len(digest) - 1 do
        table.insert(hex, string.format("%02x", buffer.readu8(digest, at)))
    end
    return table.concat(hex)
end

--- `url` into `file`, unless it is already there with the bytes `expected`
--- names. A cached file that is not is downloaded again; a download that is
--- not is refused and removed.
local function download(url: string, file: string, expected: string): string?
    if fs.exists(file) then
        if sha256Of(file) == expected then
            return nil
        end
        fs.remove(file)
    end
    step(tr("cli.android.downloading", { url = url }))
    local partial = `{file}.partial`
    if fs.exists(partial) then
        fs.remove(partial)
    end
    local fetched = process.run({ "curl", "-fL", "--retry", "3", "-o", partial, url })
    if not fetched.ok then
        return fetched.stdout .. fetched.stderr .. "\n" .. tr("cli.android.err.download", { url = url })
    end
    local found = sha256Of(partial)
    if found ~= expected then
        fs.remove(partial)
        return tr("cli.android.err.checksum", { url = url, expected = expected, found = found })
    end
    fs.copy(partial, file)
    fs.remove(partial)
    return nil
end

--- An archive into `into`. `jdk` is the JDK to fall back on for a `.zip` on
--- Linux with no `unzip`.
local function unpack(archive: string, into: string, jdk: string?): string?
    fs.createDirectory(into, { makeParents = true })
    local isZip = string.sub(archive, -4) == ".zip"
    local command: { string }
    local options: process.ProcessRunOptions? = nil
    if not isZip or platform.win32 then
        command = { "tar", "-xf", archive, "-C", into }
    elseif process.run({ "unzip", "-v" }).ok then
        command = { "unzip", "-q", "-o", archive, "-d", into }
    elseif jdk ~= nil then
        command = { projectlib.joined(jdk, "bin", "jar"), "xf", archive }
        options = { cwd = into } :: process.ProcessRunOptions
    else
        return tr("cli.android.err.no_unzip")
    end
    local unpacked = process.run(command, options)
    if not unpacked.ok then
        return unpacked.stdout .. unpacked.stderr .. "\n" .. tr("cli.android.err.unpack", { path = archive })
    end
    return nil
end

local function removeTree(target: string)
    if fs.exists(target) then
        fs.removeDirectory(target, { recursive = true })
    end
end

--- The JDK at the pinned version: `<toolchains>/jdk17/jdk-<version>`.
local function installJdk(toolchains: string, downloads: string): (string?, string?)
    local home = projectlib.joined(toolchains, "jdk17", `jdk-{JdkVersion}`)
    if fs.exists(projectlib.joined(home, "bin", exe("java"))) then
        step(tr("cli.android.present", { what = `JDK {JdkVersion}` }))
        return home, nil
    end
    local extension = if platform.win32 then "zip" else "tar.gz"
    local archive = projectlib.joined(downloads, `jdk-{JdkVersion}.{extension}`)
    local encoded = string.gsub(JdkVersion, "%+", "%%2B")
    local url = `https://api.adoptium.net/v3/binary/version/jdk-{encoded}/{osName()}/x64/jdk/hotspot/normal/eclipse`
    local failed = download(url, archive, Sha256[`jdk-{osName()}`])
    if failed ~= nil then
        return nil, failed
    end
    failed = unpack(archive, projectlib.joined(toolchains, "jdk17"), nil)
    if failed ~= nil then
        return nil, failed
    end
    if not fs.exists(projectlib.joined(home, "bin", exe("java"))) then
        return nil, tr("cli.android.err.unpack", { path = archive })
    end
    return home, nil
end

local function installGradle(toolchains: string, downloads: string, jdk: string): string?
    local home = projectlib.joined(toolchains, `gradle-{GradleVersion}`)
    if fs.exists(projectlib.joined(home, "lib")) then
        step(tr("cli.android.present", { what = `Gradle {GradleVersion}` }))
        return nil
    end
    local archive = projectlib.joined(downloads, `gradle-{GradleVersion}-bin.zip`)
    local failed =
        download(`https://services.gradle.org/distributions/gradle-{GradleVersion}-bin.zip`, archive, Sha256.gradle)
    if failed ~= nil then
        return failed
    end
    return unpack(archive, toolchains, jdk)
end

--- The command-line tools, as `cmdline-tools/latest`, which is where
--- `sdkmanager` expects to find itself.
local function installCmdlineTools(sdk: string, downloads: string, jdk: string): (string?, string?)
    local bin = projectlib.joined(sdk, "cmdline-tools", "latest", "bin")
    local sdkmanager = projectlib.joined(bin, if platform.win32 then "sdkmanager.bat" else "sdkmanager")
    if fs.exists(sdkmanager) then
        return sdkmanager, nil
    end
    local archive = projectlib.joined(downloads, `commandlinetools-{osName()}-{CmdlineToolsBuild}.zip`)
    local failed = download(
        `https://dl.google.com/android/repository/commandlinetools-{if platform.win32 then "win" else "linux"}-{CmdlineToolsBuild}_latest.zip`,
        archive,
        Sha256[`cmdline-tools-{osName()}`]
    )
    if failed ~= nil then
        return nil, failed
    end
    local staging = projectlib.joined(sdk, "cmdline-tools", "staging")
    removeTree(staging)
    failed = unpack(archive, staging, jdk)
    if failed ~= nil then
        return nil, failed
    end
    fs.createDirectory(projectlib.joined(sdk, "cmdline-tools"), { makeParents = true })
    -- The archive's one top-level folder, moved under its expected name.
    local latest = projectlib.joined(sdk, "cmdline-tools", "latest")
    removeTree(latest)
    local copied = process.run(
        if platform.win32
            then { "cmd", "/c", "move", projectlib.joined(staging, "cmdline-tools"), latest }
            else { "mv", projectlib.joined(staging, "cmdline-tools"), latest }
    )
    removeTree(staging)
    if not copied.ok or not fs.exists(sdkmanager) then
        return nil, copied.stdout .. copied.stderr .. "\n" .. tr("cli.android.err.unpack", { path = archive })
    end
    return sdkmanager, nil
end

--- Whether an SDK package is already where `sdkmanager` puts it.
local function packagePresent(sdk: string, package: string): boolean
    return fs.exists(projectlib.joined(sdk, (string.gsub(package, ";", "/"))))
end

--- The licences, then the packages. Through a shell with the answers
--- redirected into it, because `sdkmanager` reads its licence prompts from
--- stdin, and a run that could not answer them skips every package and still
--- exits zero -- so each package is checked for afterwards.
local function installPackages(sdk: string, sdkmanager: string, jdk: string, downloads: string): string?
    local missing: { string } = {}
    for _, package in SdkPackages do
        if packagePresent(sdk, package) then
            step(tr("cli.android.present", { what = package }))
        else
            table.insert(missing, package)
        end
    end
    if #missing == 0 then
        return nil
    end
    step(tr("cli.android.licences"))
    local answers = projectlib.joined(downloads, "yes.txt")
    fs.writeStringToFile(answers, string.rep("y\n", 40))
    local env = table.clone(process.env)
    env.JAVA_HOME = jdk
    local quoted = `"{table.concat(missing, '" "')}"`
    -- **As a script file, not a `cmd /c` line.** The runtime quotes each
    -- argument with backslash escapes, which `cmd` does not read -- a quoted
    -- path came through as `\"C:\...\"` and was not found. A script's own
    -- lines are passed to nobody's quoting.
    local script: string
    local command: { string }
    if platform.win32 then
        script = projectlib.joined(downloads, "sdk-install.cmd")
        fs.writeStringToFile(
            script,
            "@echo off\r\n"
                .. `call "{sdkmanager}" --sdk_root="{sdk}" --licenses < "{answers}" > nul 2>&1\r\n`
                .. `call "{sdkmanager}" --sdk_root="{sdk}" --install {quoted}\r\n`
        )
        command = { "cmd", "/c", script }
    else
        script = projectlib.joined(downloads, "sdk-install.sh")
        fs.writeStringToFile(
            script,
            `sh "{sdkmanager}" --sdk_root="{sdk}" --licenses < "{answers}" > /dev/null 2>&1\n`
                .. `sh "{sdkmanager}" --sdk_root="{sdk}" --install {quoted}\n`
        )
        command = { "sh", script }
    end
    step(tr("cli.android.installing", { packages = table.concat(missing, ", ") }))
    local installed = process.run(command, { env = env })
    for _, package in missing do
        if not packagePresent(sdk, package) then
            return installed.stdout .. installed.stderr .. "\n" .. tr("cli.android.err.package", { package = package })
        end
    end
    return nil
end

local function run(): number
    local folder = android.userFolder()
    local toolchains = projectlib.joined(folder, "toolchains")
    local downloads = projectlib.joined(toolchains, "dl")
    local sdk = android.userSdk()
    fs.createDirectory(downloads, { makeParents = true })

    local jdk, jdkError = installJdk(toolchains, downloads)
    if jdk == nil then
        print(jdkError)
        return 1
    end
    local failed = installGradle(toolchains, downloads, jdk :: string)
    if failed ~= nil then
        print(failed)
        return 1
    end
    local sdkmanager, toolsError = installCmdlineTools(sdk, downloads, jdk :: string)
    if sdkmanager == nil then
        print(toolsError)
        return 1
    end
    failed = installPackages(sdk, sdkmanager :: string, jdk :: string, downloads)
    if failed ~= nil then
        print(failed)
        return 1
    end
    print(tr("cli.android.done", { folder = folder, sdk = sdk }))
    return 0
end

return {
    run = run,
    JdkVersion = JdkVersion,
    GradleVersion = GradleVersion,
}
