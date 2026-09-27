#!/usr/bin/env python3
"""The rename of ADR 0109, as a script: the old brand out of the code.

Run from the repository root:

    python tools/repo/rename/rename.py --stage code      # R1: namespaces, includes, include dirs
    python tools/repo/rename/rename.py --stage build     # R2: LUAUG_* -> ENG_*, luaug_* -> engine_*
    python tools/repo/rename/rename.py --stage project   # R3: project files, formats, URIs, modules

**Idempotent**: a second run of a stage changes nothing, which is how a reviewer
knows the stage is the script and not a hand edit beside it. **History is not
touched**: ADRs, the CHANGELOG, the progress archive, research, the defect
register, closed briefs and the art threads say what was true when they were
written (docs/briefs/rename-kickoff.md, "What must hold").

**No compatibility with the old names** (the owner, 2026-09-26): the engine is
unreleased under its brand, and its versions restart at 0.0.1 as Ludwerk, so a
project, a scene or a variable named the old way is simply renamed -- by this
script, for everything in the repository -- rather than read both ways. The one
old name that survives is `luaug_dpow`, a C symbol the vendored, patched Luau
calls (third_party/patches/luau/0001); renaming it means re-vendoring.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]

# What is code, build or a project: everything the rename rewrites.
SCOPE = [
    "engine", "runtime", "tools", "api", "templates", "examples", "scripts", "cmake",
    "samples", "tests", "platforms", "shaders", "branding", ".github",
    "CMakeLists.txt", "CMakePresets.json", ".luaurc", ".gitignore", ".styluaignore",
    "rokit.toml", "stylua.toml", "selene.toml",
    # Ours, inside third_party/: the build glue, the manifest and its readme.
    # Never the vendored trees or their patches (R13).
    "third_party/CMakeLists.txt", "third_party/manifest.json", "third_party/README.md",
]
# Our own files inside third_party/, which the exclusion below lets through.
THIRD_PARTY_OURS = {"third_party/CMakeLists.txt", "third_party/manifest.json", "third_party/README.md"}
# Never rewritten, even inside the scope.
EXCLUDE = [
    "tools/repo/rename/",
    "third_party/",
    "scripts/gates/docs-lint.sh",  # its brand lint spells the old names on purpose
    "branding/brand.toml",  # the brand's home, and its record of the names before
]
BINARY_SUFFIXES = {
    ".png", ".ico", ".jpg", ".jpeg", ".tga", ".bmp", ".ktx2", ".dds", ".wav", ".ogg", ".mp3",
    ".ttf", ".otf", ".glb", ".bin", ".pack", ".zip", ".rar", ".jar", ".keystore", ".exe", ".dll",
    ".spv", ".dxil", ".metallib", ".a", ".lib", ".so",
}


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout


def tracked() -> list[str]:
    # Tracked files and new ones not yet added (what a stage before this one
    # created), but never what .gitignore keeps out.
    files = git("ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *SCOPE).split("\0")
    out = []
    for f in files:
        if not f or (f not in THIRD_PARTY_OURS and any(f.startswith(e) or f == e for e in EXCLUDE)):
            continue
        if pathlib.PurePosixPath(f).suffix.lower() in BINARY_SUFFIXES:
            continue
        out.append(f)
    return out


def move(old: str, new: str) -> bool:
    """`git mv`, when `old` is still there: the idempotence of a directory move."""
    if not (ROOT / old).exists() or (ROOT / new).exists():
        return False
    (ROOT / new).parent.mkdir(parents=True, exist_ok=True)
    git("mv", old, new)
    return True


def rewrite(rules: list[tuple[re.Pattern[str], str]], files: list[str]) -> int:
    changed = 0
    for f in files:
        path = ROOT / f
        if not path.is_file():
            continue
        raw = path.read_bytes()
        if b"\0" in raw:
            continue
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        new = text
        for pattern, replacement in rules:
            new = pattern.sub(replacement, new)
        if new != text:
            path.write_bytes(new.encode("utf-8"))
            changed += 1
    return changed


def rx(pattern: str) -> re.Pattern[str]:
    return re.compile(pattern)


# --- R1: the code ------------------------------------------------------------

def stage_code() -> None:
    moved = 0
    for root in ("include", "interop"):
        marker = f"/{root}/luaug/"
        files = git("ls-files", "-z", "--", "engine", "tools", "samples").split("\0")
        for d in sorted({str(pathlib.PurePosixPath(f).parent) for f in files if marker in f}):
            # d is ".../<root>/luaug/<module>" or deeper: move the "luaug" level once.
            head = d.split(marker, 1)[0] + marker.rstrip("/")
            moved += move(head, head[: -len("luaug")] + "engine")
    # The shaders' include root moves with the C++ ones: `#include
    # "luaug/surface.hlsli"` is rewritten by the same rule below.
    moved += move("shaders/include/luaug", "shaders/include/engine")
    rules = [
        (rx(r"\bnamespace luaug\b"), "namespace engine"),
        (rx(r"\bluaug::"), "engine::"),
        # Include paths and the generated tree: `luaug/<module>/...` after a quote,
        # an angle bracket or a path separator. Not `@luaug/` (a Luau module, R3)
        # and not `.luaug/` (a project's cache, R3).
        (rx(r'(?<=["<])luaug/'), "engine/"),
        (rx(r"(?<=/include/)luaug\b"), "engine"),
        (rx(r"(?<=/interop/)luaug\b"), "engine"),
        (rx(r'(?<=\}/)luaug/(?=core/)'), "engine/"),
    ]
    # A local called `engine` would hide the namespace in `engine::...` below it.
    # There was one, in the scene writer; it becomes `engineDefault`.
    rules.append((rx(r"std::optional<Value> engine = defaults\.of"), "std::optional<Value> engineDefault = defaults.of"))
    rules.append((rx(r"engine\.has_value\(\)"), "engineDefault.has_value()"))
    rules.append((rx(r"\*engine\)"), "*engineDefault)"))
    n = rewrite(rules, tracked())
    print(f"code: {moved} include root(s) moved, {n} file(s) rewritten")


# --- R2: the build -----------------------------------------------------------

def stage_build() -> None:
    moved = 0
    # Every file in the scope named `luaug_*` (the CMake modules, the shader
    # headers, the test support headers): its name follows its contents.
    for f in tracked():
        name = pathlib.PurePosixPath(f).name
        if name.startswith("luaug_"):
            moved += move(f, str(pathlib.PurePosixPath(f).with_name("engine_" + name[len("luaug_"):])))
    rules = [
        (rx(r"\bLUAUG_"), "ENG_"),
        # `-DLUAUG_PROFILE=...` on a command line: no word boundary after `-D`.
        (rx(r"(?<=-D)LUAUG_"), "ENG_"),
        # The surface-shader parser skipped the macro's name by a fixed length
        # (11 for `LUAUG_PARAM`, 13 for `LUAUG_TEXTURE`); it asks the name now.
        (rx(r"argumentsAt\(view, at \+ 11\)"), 'argumentsAt(view, at + std::string_view("ENG_PARAM").size())'),
        (rx(r"argumentsAt\(view, at \+ 13\)"), 'argumentsAt(view, at + std::string_view("ENG_TEXTURE").size())'),
        # Gradle's names for the player's signing and build root.
        (rx(r"\bsigningConfigs\.luaug\b"), "signingConfigs.engine"),
        (rx(r"(?<![A-Za-z0-9_])luaug\.(?=(appName|applicationId|buildRoot|keyAlias|keyPassword|keystore|orientation"
            r"|resDir|stageDir|storePassword|versionCode|versionName)\b)"), "engine."),
        (rx(r"\bluaugdebugkey\b"), "enginedebugkey"),
        # Internal CamelCase names -- the shaders' `LuaugExposureKey`, the
        # surface wrapper's `LuaugSceneColor` -- and the reserved prefix of a
        # surface parameter's name.
        (rx(r"\bLuaug(?=[A-Z])"), "Engine"),
        (rx(r"\bluaug(?=[A-Z][a-z])"), "engine"),
        (rx(r'rfind\("Luaug", 0\)'), 'rfind("Engine", 0)'),
        (rx(r"(?<![A-Za-z0-9])_luaug_"), "_engine_"),
        # Not `luaug_dpow`: the vendored, patched Luau calls it by that name
        # (third_party/patches/luau/0001), and renaming it means re-vendoring.
        # It is a frozen C symbol, listed as such in the brand lint.
        (rx(r"(?<![A-Za-z0-9])luaug_(?!dpow)"), "engine_"),
        # Executables, containers, volumes and temporary names: `luaug-<thing>`.
        # The file-format tags (`luaug-scene` and friends) are R3's, and are
        # listed there so they are renamed with their readers.
        (rx(r"(?<![A-Za-z0-9.])luaug-(?!(scene|material|content-manifest|chunk-index|chunk-source|global|test-report|projects|partition)\b)"), "engine-"),
    ]
    n = rewrite(rules, tracked())
    print(f"build: {moved} file(s) moved, {n} file(s) rewritten")


# --- R3: a game project --------------------------------------------------------

FORMATS = ["scene", "material", "content-manifest", "chunk-index", "chunk-source", "global", "projects", "partition"]


def stage_project() -> None:
    moved = 0
    moved += move("runtime/luaug", "runtime/engine")
    for f in git("ls-files", "-z", "--", "examples", "templates", "tests", "samples", "tools").split("\0"):
        if pathlib.PurePosixPath(f).name == "luaug.toml":
            moved += move(f, str(pathlib.PurePosixPath(f).with_name("project.toml")))
    # The Android player's Java package directory.
    for f in git("ls-files", "-z", "--", "platforms", "samples").split("\0"):
        if "/org/luaug/" in f:
            base = f.split("/org/luaug/", 1)[0]
            moved += move(base + "/org/luaug", base + "/engine")
    rules = [
        # File-format tags, in files and in the code that writes them. The
        # readers accept the old tag too (legacy_names.h).
        *[(rx(rf'"luaug-{fmt}"'), f'"{fmt}"') for fmt in FORMATS],
        (rx(r"\bluaug-test-report\.json\b"), "test-report.json"),
        # Built-in content: `luaug://primitive/block` -> `engine://...`.
        (rx(r"\bluaug://"), "engine://"),
        # The project file, the cache, the log.
        (rx(r"(?<![A-Za-z0-9_-])luaug\.toml\b"), "project.toml"),
        (rx(r"(?<![A-Za-z0-9_])\.luaug(?![A-Za-z0-9_])"), ".engine"),
        (rx(r"(?<![A-Za-z0-9_-])luaug\.log\b"), "engine.log"),
        # Luau modules: `@luaug/camera`, the `.luaurc` alias and its directory.
        (rx(r"@luaug\b"), "@engine"),
        (rx(r'"luaug"(\s*:\s*)"runtime/luaug"'), r'"engine"\1"runtime/engine"'),
        (rx(r"\bruntime/luaug\b"), "runtime/engine"),
        # The same folder built in parts (`"runtime" / "luaug"`), and every other
        # `"luaug"` standing alone in quotes: a folder, a module, a log tag --
        # technical names, never the shop window.
        (rx(r"(?<=[\"'])luaug(?=[\"'])"), "engine"),
        # The surface-shader API (ADR 0091): the header and its names. A shim
        # at the old path keeps an existing shader compiling (legacy).
        (rx(r"(?<=[\"</])luaug/surface\.hlsli"), "engine/surface.hlsli"),
        (rx(r"\bluaug(?=[A-Z][a-z])"), "engine"),
        # The same names where comments and messages mention them unquoted.
        (rx(r"\bluaug/surface\.hlsli\b"), "engine/surface.hlsli"),
        (rx(r"\bluaug/(?=(app|asset|audio|core|input|jobs|nav|net|physics|platform|render|replication|rhi|scene|script|ui)/)"),
         "engine/"),
        (rx(r"\bluaug-(content-manifest|chunk-index|chunk-source|scene|material|global|projects|partition)\b"), r"\1"),
        # The CLI's reading of the engine's version: `version.luaug()`.
        (rx(r"\bversion\.luaug\b"), "version.engine"),
        (rx(r"\blocal function luaug\(\): Reading"), "local function engineVersion(): Reading"),
        (rx(r"= luaug\(\)"), "= engineVersion()"),
        (rx(r"(?m)^    luaug = luaug,$"), "    engine = engineVersion,"),
        # The Android player's Java package and Gradle property.
        (rx(r"\borg\.luaug\.player\b"), "engine.player"),
        (rx(r"\borg\.luaug\.triangle\b"), "engine.triangle"),
        (rx(r"\borg/luaug/"), "engine/"),
        (rx(r"-Pluaug\."), "-Pengine."),
        (rx(r"(?<![A-Za-z0-9_])luaug\.stageDir\b"), "engine.stageDir"),
    ]
    n = rewrite(rules, tracked())
    print(f"project: {moved} file(s) moved, {n} file(s) rewritten")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", choices=["code", "build", "project"], required=True)
    args = parser.parse_args()
    {"code": stage_code, "build": stage_build, "project": stage_project}[args.stage]()
    return 0


if __name__ == "__main__":
    sys.exit(main())
