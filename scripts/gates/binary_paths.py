"""A shipped binary names no machine: the gate that reads one for the paths it
was built at.

    python3 scripts/gates/binary_paths.py <binary>... --forbid <path>...

Every `--forbid` path is looked for in each binary's bytes as it is written,
with its separators turned the other way, and on Windows without regard to
case: the source tree, the build tree and the user's folder of the machine
that built it. A hit is `__FILE__` in an assertion, a debug path, a program
database's folder -- each one the build machine's folders and its user's name,
in every copy of every game. Any hit fails, with the strings found.
"""

import re
import sys


def forms(path: str) -> list[bytes]:
    trimmed = path.rstrip("/\\")
    if len(trimmed) < 4:
        return []
    out = {trimmed, trimmed.replace("/", "\\"), trimmed.replace("\\", "/")}
    # Git Bash hands a native program `C:/Users/...`; a binary says `C:\Users\...`.
    return sorted({form.encode("utf-8") for form in out})


def main(argv: list[str]) -> int:
    binaries: list[str] = []
    forbidden: list[str] = []
    at = 0
    while at < len(argv):
        if argv[at] == "--forbid" and at + 1 < len(argv):
            forbidden.append(argv[at + 1])
            at += 2
            continue
        binaries.append(argv[at])
        at += 1
    if not binaries or not forbidden:
        print(__doc__)
        return 2
    # A path, and not the start of a longer name: `/repo` is in
    # `/report_descriptor` and is not a folder of anybody's.
    patterns = [
        re.compile(
            re.escape(form) + rb"(?![A-Za-z0-9_.-])[ -~]*", re.IGNORECASE if sys.platform == "win32" else 0
        )
        for path in forbidden
        for form in forms(path)
    ]
    failed = False
    for binary in binaries:
        with open(binary, "rb") as handle:
            data = handle.read()
        hits: dict[bytes, int] = {}
        for pattern in patterns:
            for match in pattern.finditer(data):
                hits[match.group(0)[:160]] = hits.get(match.group(0)[:160], 0) + 1
        if hits:
            failed = True
            print(f"binary-paths: {binary} names the machine it was built on, {sum(hits.values())} times:")
            for text, count in sorted(hits.items())[:20]:
                print(f"    {count:4d}  {text.decode('utf-8', 'replace')}")
        else:
            print(f"binary-paths: {binary}: none of {len(forbidden)} build paths")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
