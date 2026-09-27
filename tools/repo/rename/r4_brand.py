#!/usr/bin/env python3
"""Stage R4 of the rename (ADR 0109): the shop window says the brand.

Run from the repository root, after `rename.py --stage project`:

    python tools/repo/rename/r4_brand.py

- The version restarts: `project(Engine VERSION 0.0.1)`.
- The files named for the old brand take generic names (`branding/app.rc`,
  `branding/icon/icon.ico`, `branding/mark.svg`), except the command's wrappers,
  which are the brand's short name (`scripts/ludwerk.ps1`): a person types it.
- What a person reads as the product's name comes from `branding/brand.toml`:
  the window title, the About box, the user folder, the package folder, the
  documentation site's name. Comments name the brand as prose.

Idempotent.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rename  # noqa: E402  (the same scope, exclusions and helpers)


def read(rel: str) -> str:
    return (ROOT / rel).read_bytes().decode("utf-8")


def write(rel: str, text: str) -> None:
    (ROOT / rel).write_bytes(text.encode("utf-8"))


def edit(rel: str, old: str, new: str, marker: str, count: int = 1) -> None:
    text = read(rel)
    if marker in text:
        return
    if text.count(old) != count:
        sys.exit(f"r4: {rel}: expected {count} of {old[:70]!r}, found {text.count(old)}")
    write(rel, text.replace(old, new))


def brand_short() -> str:
    for line in read("branding/brand.toml").splitlines():
        m = re.match(r'^short\s*=\s*"([^"]*)"', line)
        if m:
            return m.group(1)
    sys.exit("r4: branding/brand.toml has no short name")


def replace_or_move(old: str, new: str) -> int:
    """Move `old` to `new`; when `new` is already there -- new artwork drawn under
    the generic name -- the old file is removed from the tree instead."""
    if (ROOT / old).exists() and (ROOT / new).exists():
        # Only when the two are the same bytes: then removing the old name loses
        # nothing, whatever git thinks of its working-tree changes.
        if (ROOT / old).read_bytes() != (ROOT / new).read_bytes():
            sys.exit(f"r4: {old} and {new} differ; decide which is the artwork before renaming")
        rename.git("rm", "-q", "-f", old)
        return 1
    return int(rename.move(old, new))


def moves() -> int:
    short = brand_short()
    n = 0
    for ext in ("ps1", "sh", "cmd"):
        n += rename.move(f"scripts/luaug.{ext}", f"scripts/{short}.{ext}")
    n += replace_or_move("branding/luaug.rc", "branding/app.rc")
    for f in rename.git("ls-files", "-z", "--", "branding").split("\0"):
        p = pathlib.PurePosixPath(f)
        if p.name.startswith("luaug-"):
            rest = p.name[len("luaug-"):]
            if p.parent.name == "icon":
                rest = rest  # `luaug-128.png` -> `128.png`
            n += replace_or_move(f, str(p.with_name(rest)))
        elif p.name in ("luaug.ico", "luaug.svg"):
            n += replace_or_move(f, str(p.with_name("icon" + p.suffix)))
    return n


def rules() -> list[tuple[re.Pattern[str], str]]:
    rx = rename.rx
    short = brand_short()
    return [
        # The version restarts, and the project's name is the code's.
        (rx(r"\bproject\(LuauG\b"), "project(Engine"),
        (rx(r"project%\(LuauG"), "project%(Engine"),
        (rx(r"project\(LuauG VERSION"), "project(Engine VERSION"),
        # Files that took generic names.
        (rx(r"\bbranding/luaug\.rc\b"), "branding/app.rc"),
        (rx(r"(?<![A-Za-z0-9_-])luaug\.rc\b"), "app.rc"),
        (rx(r"\bicon/luaug\.(ico|svg)\b"), r"icon/icon.\1"),
        (rx(r"(?<![A-Za-z0-9_-])luaug\.ico\b"), "icon.ico"),
        (rx(r"\bicon/luaug-(\d+)\.png\b"), r"icon/\1.png"),
        (rx(r"\bluaug-(?=(mark|lockup|logo|app-icon|social-card)\b)"), ""),
        # The command's wrappers.
        (rx(r"\bscripts/luaug\.(ps1|sh|cmd)\b"), rf"scripts/{short}.\1"),
        (rx(r"(?<![A-Za-z0-9_/-])luaug\.(ps1|sh|cmd)\b"), rf"{short}.\1"),
        # Identifiers that are data but not the brand.
        (rx(r'"luaug\.(instances|content)"'), r'"engine.\1"'),
        (rx(r"\bdev\.local\.luaug-test\b"), "dev.local.engine-test"),
        (rx(r"\bdev\.luaug\.my-game\b"), "com.example.my-game"),
        (rx(r"\bdev\.luaug\."), f"dev.{short}."),
        # Messages that named the product where the product is beside the point.
        (rx(r'"LuauG: '), '"engine: '),
        (rx(r"All declared LuauG modules"), "All declared engine modules"),
        (rx(r'"not a LuauG '), '"not a '),
        (rx(r"LuauG hand-authored test fixture"), "Hand-authored test fixture"),
        (rx(r"LuauG M6 skinned-bar fixture"), "M6 skinned-bar fixture"),
        (rx(r"Hello from LuauG"), "Hello from the engine"),
        (rx(r'"LuauG \{version\}'), '"Engine {version}'),
        (rx(r'"LuauG 0\.0\.1'), '"Engine 0.0.1'),
        # Technical names that are not the shop window: the Android log tag, a
        # Gradle property's environment spelling, paths in comments.
        (rx(r"(__android_log_write\([^,]+, )\"(?:luaug|ludwerk)\""), r'\1"engine"'),
        (rx(r"\bluaug:V\b"), "engine:V"),
        (rx(r"\bORG_GRADLE_PROJECT_luaug\."), "ORG_GRADLE_PROJECT_engine."),
        (rx(r"\bluaug/"), "engine/"),
        # A native message's prefix: the brand's short name, from the header.
        (rx(r'static const char prefix\[\] = "luaug: died on signal ";'),
         'static const char prefix[] = ENG_BRAND_SHORT ": died on signal ";'),
        (rx(r'"luaug: the packaged game could not be extracted: "'),
         'std::string(core::kBrandShort) + ": the packaged game could not be extracted: "'),
        (rx(r"'LuauG Game'"), "'Game'"),
        # A crash handler's headline: the literal macro, which needs no allocation.
        (rx(r'std::string\("LuauG (terminated|died)'), r'std::string(ENG_BRAND_NAME " \1'),
        (rx(r'"LuauG (terminated|died)'), r'ENG_BRAND_NAME " \1'),
        # PowerShell: the user folder and the package folder, by the brand's name.
        (rx(r"'LuauG\\build'"), lambda m: '"$BrandName\\build"'),
        (rx(r'"LuauG\\build"'), lambda m: '"$BrandName\\build"'),
        (rx(r"-Filter 'LuauG-\*'"), '-Filter "$BrandName-*"'),
        # The CLI's user folder.
        (rx(r'joined\(appData, "LuauG", "LuauG"\)'), "joined(appData, Brand.read().Name, Brand.read().Name)"),
        (rx(r'joined\(appData, "LuauG"\)'), "joined(appData, Brand.read().Name)"),
        (rx(r'"share", "LuauG"\)'), '"share", Brand.read().Name)'),
        # The documentation site and the protocol's title.
        (rx(r'"# LuauG API reference"'), '"# " .. Brand.read().Name .. " API reference"'),
        (rx(r"`# The LuauG wire protocol, version \{Wire\.ProtocolVersion\}`"),
         "`# The {Brand.read().Name} wire protocol, version {Wire.ProtocolVersion}`"),
        (rx(r'"<h1>LuauG documentation</h1>"'), '"<h1>" .. Brand.read().Name .. " documentation</h1>"'),
        (rx(r'local SiteName = "LuauG"'), "local SiteName = Brand.read().Name"),
        (rx(r'\{ Slug = "what-is-luaug", Title = "What LuauG is" \}'), '{ Slug = "introduction", Title = "Introduction" }'),
        (rx(r"\bwhat-is-luaug\b"), "introduction"),
        # The CLI reads the version from the project() line, whose name is the code's.
        (rx(r"(project%s\*%\(%s\*)LuauG"), r"\1Engine"),
        # The testing module's failure marker, and the CLI's own fallback prints.
        (rx(r"\bluaug\.testing\b"), "engine.testing"),
        (rx(r"print\(`luaug: "), "print(`{Brand.read().Short}: "),
        (rx(r"\bscripts/luaug\.\*"), f"scripts/{short}.*"),
        (rx(r'options\.name or "LuauG Developer"'), 'options.name or (Brand.read().Name .. " Developer")'),
        (rx(r'throw "no LuauG-\* folder under \$root"'), 'throw "no $BrandName-* folder under $root"'),
        (rx(r'\(`previous = \["LuauG"\]`\)'), '(`previous = ["Old", "Older"]`)'),
        (rx(r"\bLUAUG\b"), "LUDWERK"),
        # The last two: whatever is left names the product as prose -- comments,
        # a usage line, a docs string -- which says the brand.
        (rx(r"\bluaug\b(?![_.\-/:])"), short),
        (rx(r"\bLuauG\b"), "Ludwerk"),
    ]


def data_edits() -> None:
    """Where the product's name is data, it comes from the brand file."""
    # CMake: the version restarts.
    text = read("CMakeLists.txt")
    text = re.sub(r"(project\(Engine\s+VERSION\s+)[0-9.]+", r"\g<1>0.0.1", text)
    write("CMakeLists.txt", text)

    # C++: the user folder, the window title, the About box.
    edit("engine/platform/src/platform.cpp", 'SDL_GetPrefPath("Ludwerk", "Ludwerk")',
         "SDL_GetPrefPath(std::string(core::kBrandName).c_str(), std::string(core::kBrandName).c_str())",
         "SDL_GetPrefPath(std::string(core::kBrandName)")
    for rel in ("engine/platform/src/platform.cpp", "engine/platform/src/crash.cpp"):
        text = read(rel)
        if '#include "engine/core/brand.h"' not in text:
            first = text.index("#include")
            eol = text.index("\n", first)
            write(rel, text[: eol + 1] + '\n#include "engine/core/brand.h"\n' + text[eol + 1:])
    edit("engine/app/src/engine.cpp", 'std::string("Ludwerk")', "std::string(core::kBrandName)",
         "std::string(core::kBrandName)")
    edit("engine/app/src/engine.cpp", '#include "engine/app/engine.h"\n',
         '#include "engine/app/engine.h"\n\n#include "engine/core/brand.h"\n', '#include "engine/core/brand.h"')
    ov = "engine/app/src/debug_overlay.cpp"
    text = read(ov)
    if "kAboutTitle" not in text:
        text = text.replace('iconMenuItem(icons, icons::ActionInformation, "About Ludwerk")',
                            "iconMenuItem(icons, icons::ActionInformation, kAboutTitle.c_str())")
        text = text.replace('ImGui::OpenPopup("About Ludwerk");', "ImGui::OpenPopup(kAboutTitle.c_str());")
        text = text.replace('beginEditorDialog("About Ludwerk", 430.0f', "beginEditorDialog(kAboutTitle.c_str(), 430.0f")
        text = text.replace('ImGui::TextUnformatted("Ludwerk");', "ImGui::TextUnformatted(std::string(core::kBrandName).c_str());")
        text = text.replace('ImGui::Begin("Ludwerk")', "ImGui::Begin(std::string(core::kBrandName).c_str())")
        anchor = "constexpr const char* kInstanceDragPayload"
        text = text.replace(anchor, "// The About box's title: the product's name, from branding/brand.toml (ADR 0109).\n"
                                    "const std::string kAboutTitle = \"About \" + std::string(core::kBrandName);\n\n" + anchor, 1)
        if '#include "engine/core/brand.h"' not in text:
            first = text.index("#include")
            eol = text.index("\n", first)
            text = text[: eol + 1] + '\n#include "engine/core/brand.h"\n' + text[eol + 1:]
        write(ov, text)


BRAND_PS1 = r"""# The product's name, from branding/brand.toml (ADR 0109): the one place it
# lives. Dot-sourced by the scripts that name a folder after the product -- the
# build root under the user's folder, the package folder.
$BrandName = Get-Content (Join-Path $PSScriptRoot '..\..\branding\brand.toml') |
    Where-Object { $_ -match '^name\s*=\s*"([^"]*)"' } |
    ForEach-Object { $Matches[1] } |
    Select-Object -First 1
"""


def ps_insertion_point(text: str) -> int:
    """Just after a script-level `param(...)`, or after the leading comments."""
    m = re.search(r"(?im)^\s*param\s*\(", text)
    if m:
        before = text[: m.start()]
        if "<#" in before or all(not l.strip() or l.strip().startswith(("#", "[")) for l in before.splitlines()):
            depth, i = 0, text.index("(", m.start())
            while i < len(text):
                if text[i] == "(":
                    depth += 1
                elif text[i] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            return text.index("\n", i) + 1
    offset, in_block = 0, False
    for line in text.split("\n"):
        s = line.strip()
        if s.startswith("<#"):
            in_block = True
        if in_block:
            in_block = "#>" not in s
        elif s and not s.startswith("#"):
            return offset
        offset += len(line) + 1
    return offset


def requires() -> None:
    """The scripts that now read the brand get what they read it from."""
    lib = ROOT / "scripts/lib"
    lib.mkdir(exist_ok=True)
    if not (lib / "brand.ps1").exists():
        (lib / "brand.ps1").write_bytes(BRAND_PS1.encode("utf-8"))
    for f in rename.git("ls-files", "-z", "--", "scripts").split("\0"):
        if not f.endswith(".ps1") or f.startswith("scripts/lib/"):
            continue
        text = read(f)
        if "$BrandName" in text and "lib/brand.ps1" not in text:
            at = ps_insertion_point(text)
            write(f, text[:at] + '. "$PSScriptRoot/lib/brand.ps1"\n' + text[at:])
    for f in rename.git("ls-files", "-z", "--", "tools", "api").split("\0"):
        if not f.endswith(".luau") or f == "tools/cli/brand.luau" or f.startswith("tools/repo/rename/"):
            continue
        text = read(f)
        if "Brand.read()" not in text or re.search(r"^local Brand = require", text, re.M):
            continue
        here = pathlib.PurePosixPath(f).parent
        if str(here) == "tools/cli":
            spec = "./brand"
        elif str(here).startswith("tools/cli/"):
            spec = "/".join([".."] * (len(here.parts) - 2)) + "/brand"
        else:
            spec = "/".join([".."] * len(here.parts)) + "/tools/cli/brand"
        # After the last top-level require, or after `--!strict`.
        lines = text.split("\n")
        at = 0
        for i, line in enumerate(lines):
            if re.match(r"^local \w+ = require\(", line):
                at = i + 1
        if at == 0:
            at = 1 if lines and lines[0].startswith("--!") else 0
        lines.insert(at, f'local Brand = require("{spec}")')
        write(f, "\n".join(lines))


def main() -> int:
    moved = moves()
    n = rename.rewrite(rules(), rename.tracked())
    data_edits()
    requires()
    # The requires it added, in StyLua's order.
    subprocess.run(["stylua", "tools", "api", "runtime"], cwd=ROOT, check=False)
    print(f"r4: {moved} file(s) moved, {n} file(s) rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
