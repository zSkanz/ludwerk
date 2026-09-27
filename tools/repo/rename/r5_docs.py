#!/usr/bin/env python3
"""Stage R5 of the rename (ADR 0109): the current documents say Ludwerk.

Run from the repository root, after r4_brand.py:

    python tools/repo/rename/r5_docs.py

The documents a person reads to use the engine today -- the README, the manual,
the design documents, the open ledgers, the agents' guide -- say the brand and
the new names. **History is not rewritten**: ADRs, closed briefs, the progress
archive, research, the defect register and the CHANGELOG's released sections
say what was true when they were written. Idempotent.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import rename  # noqa: E402

CURRENT = [
    "README.md", "CLAUDE.md", "CONTRIBUTING.md", "MASTER_PROMPT.md",
    "docs/manual", "docs/api-design.md", "docs/architecture.md",
    "docs/briefs/views-kickoff.md", "docs/briefs/ai-kickoff.md", "docs/briefs/rename-kickoff.md",
    "branding/README.md", "icons/README.md",
]


def rules() -> list[tuple[re.Pattern[str], object]]:
    rx = rename.rx
    return [
        # The code's names.
        (rx(r"\bnamespace luaug\b"), "namespace engine"),
        (rx(r"\bluaug::"), "engine::"),
        (rx(r"\binclude/luaug\b"), "include/engine"),
        (rx(r"\binterop/luaug\b"), "interop/engine"),
        (rx(r"\bLUAUG_"), "ENG_"),
        (rx(r"(?<=-D)LUAUG_"), "ENG_"),
        (rx(r"\bLuaug(?=[A-Z])"), "Engine"),
        (rx(r"(?<![A-Za-z0-9])luaug_(?!dpow)"), "engine_"),
        # A project's names.
        (rx(r"(?<![A-Za-z0-9_-])luaug\.toml\b"), "project.toml"),
        (rx(r"(?<![A-Za-z0-9_])\.luaug(?![A-Za-z0-9_])"), ".engine"),
        (rx(r"(?<![A-Za-z0-9_-])luaug\.log\b"), "engine.log"),
        (rx(r"@luaug\b"), "@engine"),
        (rx(r"\bruntime/luaug\b"), "runtime/engine"),
        (rx(r"\bluaug://"), "engine://"),
        (rx(r"\bluaug-(scene|material|content-manifest|chunk-index|chunk-source|global|projects|partition)\b"), r"\1"),
        (rx(r"\bluaug-test-report\.json\b"), "test-report.json"),
        (rx(r"\bluaug/"), "engine/"),
        (rx(r"\bluaug(?=[A-Z][a-z])"), "engine"),
        # Executables and wrappers.
        (rx(r"\bscripts/luaug\.(ps1|sh|cmd|\*)"), r"scripts/ludwerk.\1"),
        (rx(r"(?<![A-Za-z0-9_/-])luaug\.(ps1|sh|cmd)\b"), r"ludwerk.\1"),
        (rx(r"(?<![A-Za-z0-9.])luaug-(?=host|triangle|tier2|shipping|lavapipe|crash|editor|build)"), "engine-"),
        # Branding files and the manual's first page.
        (rx(r"\bbranding/luaug\.rc\b"), "branding/app.rc"),
        (rx(r"\bicon/luaug\.(ico|svg)\b"), r"icon/icon.\1"),
        (rx(r"\bicon/luaug-(\d+)\.png\b"), r"icon/\1.png"),
        (rx(r"\bluaug-(?=(mark|lockup|logo|app-icon|social-card)\b)"), ""),
        (rx(r"\bwhat-is-luaug\b"), "introduction"),
        (rx(r"\bdev\.luaug\."), "dev.ludwerk."),
        # The repository itself keeps its name until the owner renames it.
        (rx(r"(github\.com/zSkanz/)(?:LuauG|luaug)\b"), r"\1LuauG"),
        # Whatever is left is the product or its command, as prose.
        (rx(r"(?<!zSkanz/)\bluaug\b(?![_.\-/:])"), "ludwerk"),
        (rx(r"(?<!zSkanz/)\bLuauG\b(?!\])"), "Ludwerk"),
    ]


def changelog() -> None:
    """Ludwerk's history starts at 0.0.1; the LuauG releases stay, under a heading."""
    path = ROOT / "CHANGELOG.md"
    text = path.read_bytes().decode("utf-8")
    if "## Before Ludwerk" in text:
        return
    text = text.replace("# Changelog\n\nEvery release of LuauG.", "# Changelog\n\nEvery release of Ludwerk.", 1)
    first_release = re.search(r"^## \[\d+\.\d+\.\d+\]", text, re.M)
    if first_release is None:
        sys.exit("r5: CHANGELOG.md has no released section")
    history = (
        "## Before Ludwerk\n\n"
        "The engine was called **LuauG** until 2026-09-26, and released three versions under\n"
        "that name. Ludwerk's own versions start again at 0.0.1 (ADR 0109): the releases\n"
        "below are its history, kept as they were written.\n\n"
    )
    text = text[: first_release.start()] + history + text[first_release.start():]
    unreleased = text.index("## [Unreleased]")
    eol = text.index("\n", unreleased)
    note = (
        "\n\n**The first version as Ludwerk, 0.0.1** (ADR 0109). The engine's name, the command and a\n"
        "project's files change, with no compatibility for the old ones: rename a project made before\n"
        "by hand -- `luaug.toml` to `project.toml`, `.luaug/` to `.engine/` (or delete it; it is a cache),\n"
        "`require(\"@luaug/...\")` to `require(\"@engine/...\")`, a surface shader's\n"
        "`#include \"luaug/surface.hlsli\"` and `LUAUG_*` macros to `engine/surface.hlsli` and `ENG_*`,\n"
        "and any `LUAUG_*` environment variable to `ENG_*`. The command is `ludwerk`.\n"
    )
    text = text[: eol] + note + text[eol:]
    path.write_bytes(text.encode("utf-8"))


def lint() -> None:
    """The brand lint fails now: nothing in code may name the old brand."""
    path = ROOT / "scripts/gates/docs-lint.sh"
    text = path.read_bytes().decode("utf-8")
    if "':(exclude)tools/repo/rename'" in text:
        return
    start = text.index("# **The brand lives in branding/brand.toml")
    end = text.index('if [[ $status -eq 0 ]]; then\n    echo "docs-lint: ok"')
    block = (pathlib.Path(__file__).resolve().parent / "r5" / "docs_lint_brand.sh").read_text(encoding="utf-8")
    path.write_bytes((text[:start] + block + text[end:]).encode("utf-8"))


def main() -> int:
    lint()
    moved = rename.move("docs/manual/get-started/what-is-luaug.md", "docs/manual/get-started/introduction.md")
    files = [f for f in rename.git("ls-files", "-z", "--", *CURRENT).split("\0") if f.endswith(".md")]
    n = rename.rewrite(rules(), files)
    changelog()
    print(f"r5: {moved} file(s) moved, {n} document(s) rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main())
