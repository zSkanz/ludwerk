#!/usr/bin/env python3
"""Stage R0 of the rename (ADR 0109): the brand gets one home, and nothing moves.

Run from the repository root, BEFORE `rename.py --stage code`:

    python tools/repo/rename/r0_brand.py

It puts `branding/brand.toml` in place (still saying the old name, so R0
changes no behaviour), makes CMake read it into a generated header, lets the
engine's and the CLI's message catalogs fill `{brand}` (and the CLI's `{tool}`)
themselves, turns the messages that spell the product's name into those
placeholders, and adds the brand lint as a report. Idempotent: every edit checks
whether it is already there.
"""

from __future__ import annotations

import pathlib
import re
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
HERE = pathlib.Path(__file__).resolve().parent / "r0"


def read(rel: str) -> str:
    return (ROOT / rel).read_bytes().decode("utf-8")


def write(rel: str, text: str) -> None:
    (ROOT / rel).write_bytes(text.encode("utf-8"))


def insert_once(rel: str, anchor: str, addition: str, marker: str, after: bool = True) -> None:
    text = read(rel)
    if marker in text:
        return
    if text.count(anchor) != 1:
        sys.exit(f"r0: {rel}: expected exactly one anchor {anchor[:60]!r}")
    write(rel, text.replace(anchor, anchor + addition if after else addition + anchor))


def replace_once(rel: str, old: str, new: str, marker: str) -> None:
    text = read(rel)
    if marker in text:
        return
    if text.count(old) != 1:
        sys.exit(f"r0: {rel}: expected exactly one {old[:60]!r}")
    write(rel, text.replace(old, new))


def main() -> int:
    # --- the files that are new ---------------------------------------------
    for src, dst in [("brand.toml", "branding/brand.toml"), ("brand.h.in", "cmake/brand.h.in"),
                     ("brand.luau.txt", "tools/cli/brand.luau")]:
        if not (ROOT / dst).exists():
            shutil.copyfile(HERE / src, ROOT / dst)

    # --- CMake reads the brand into a generated header -----------------------
    insert_once("cmake/luaug_provenance.cmake", "add_library(luaug_build_info INTERFACE)",
                (HERE / "provenance_brand.cmake").read_text(encoding="utf-8"), "_luaug_brand_file", after=False)
    replace_once("cmake/luaug_provenance.cmake", 'message(STATUS "LuauG: Luau',
                 'message(STATUS "${LUAUG_BRAND_NAME}: Luau', "${LUAUG_BRAND_NAME}: Luau")
    replace_once("engine/core/CMakeLists.txt", "    PUBLIC_DEPS\n        luaug_dmath\n    PRIVATE_DEPS",
                 "    PUBLIC_DEPS\n        luaug_dmath\n        luaug::build_info\n    PRIVATE_DEPS", "luaug::build_info")

    # --- the engine's catalog fills {brand} ----------------------------------
    replace_once("engine/core/src/i18n.cpp", '#include "luaug/core/i18n.h"\n\n#include "luaug/core/json.h"',
                 '#include "luaug/core/i18n.h"\n\n#include "luaug/core/brand.h"\n#include "luaug/core/json.h"',
                 "luaug/core/brand.h")
    replace_once("engine/core/src/i18n.cpp",
                 "        if (found != nullptr)\n            out.append(found->value());\n        else\n",
                 (HERE / "i18n_brand.cpp.txt").read_text(encoding="utf-8"), "kBrandName")
    text = read("i18n/en.json")
    text = text.replace('"LuauG {version}', '"{brand} {version}').replace("is not a LuauG ", "is not a {brand} ")
    write("i18n/en.json", text)

    # --- the CLI's catalog fills {brand} and {tool} --------------------------
    replace_once("tools/cli/catalog.luau", 'local process = require("@std/process")\n',
                 'local process = require("@std/process")\n\nlocal Brand = require("./brand")\n', "./brand")
    cat = read("tools/cli/catalog.luau")
    if "Brand.read()" not in cat:
        start = cat.index("local function tr(key: string")
        end = cat.index("\nend\n", start) + len("\nend\n")
        write("tools/cli/catalog.luau", cat[:start] + (HERE / "catalog_tr.luau.txt").read_text(encoding="utf-8") + cat[end:])
    cli = read("tools/cli/i18n/en.json")
    cli = cli.replace("LuauG", "{brand}")
    cli = re.sub(r"(?<![\w.\-/@{])luaug(?![\w.\-/}])", "{tool}", cli)
    write("tools/cli/i18n/en.json", cli)

    # --- a test that {brand} is filled -------------------------------------
    t = "engine/core/tests/i18n_tests.cpp"
    if "kBrandName" not in read(t):
        text = read(t).replace('#include "luaug/core/i18n.h"\n',
                               '#include "luaug/core/i18n.h"\n\n#include "luaug/core/brand.h"\n', 1)
        text = text.replace('"greeting": "LuauG {version} - engine initialized.",',
                            '"greeting": "Engine {version} - engine initialized.",\n'
                            '        "branded": "{brand} {version}, run with {brandShort}",')
        # Every expectation built on that message, and the other test data that
        # spelled the old name.
        text = text.replace('"LuauG ', '"Engine ')
        anchor = '    SUBCASE("substitutes a repeated placeholder every time")'
        if text.count(anchor) != 1:
            sys.exit("r0: i18n_tests.cpp anchor")
        text = text.replace(anchor, (HERE / "i18n_test_brand.cpp.txt").read_text(encoding="utf-8") + anchor)
        write(t, text)

    # --- the brand lint, as a report -----------------------------------------
    insert_once("scripts/gates/docs-lint.sh", 'if [[ $status -eq 0 ]]; then\n    echo "docs-lint: ok"',
                (HERE / "docs_lint_brand.sh").read_text(encoding="utf-8"), "== brand (ADR 0109) ==", after=False)
    print("r0: done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
