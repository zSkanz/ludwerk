#!/usr/bin/env bash
# The documentation gate: relative links resolve, pinned versions are named
# consistently, no stray references to the platform R7 keeps us clear of.
#
# This file is the gate. `.github/workflows/ci.yml` runs it and so does
# `scripts/localgate.ps1`, which is the point: the logic used to live only in
# the workflow, so running it locally meant copying commands, and a copy drifts
# until the two disagree about whether the repository is green.
#
# Run from the repository root. Exits non-zero on the first category that fails,
# after reporting every violation in it.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

# Annotations when GitHub is listening, plain text when a human is -- but the
# path is printed either way. A first draft dropped it locally, on the reasoning
# that `file=` was "the annotation", and produced two identical FAIL lines that
# named nothing.
err() { # message, [file]
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        echo "::error ${2:+file=$2}::$1"
    fi
    echo "  FAIL: ${2:+$2: }$1" >&2
}

status=0

# --- Relative links resolve -------------------------------------------------
# NOTE ON `--exclude-dir=third_party` THROUGHOUT: every check here is about
# documentation *we* authored. Vendored upstream trees are governed by ADR 0021
# (pinned, never edited in place, R13), carry their own licenses, and
# legitimately contain both broken relative links and the name R7 sweeps for
# (Luau's upstream is that platform's). Linting them would be meaningless and a
# permanent source of
# false failures.
echo "== relative links =="
# **A link resolves when what it names is TRACKED, under exactly that name.**
# The test used to be whether the path exists on the machine running it, and
# two things made that pass where a clone fails: a file system that ignores
# case took `roadmap.md` for `ROADMAP.md`, and an untracked file in a
# checkout resolved a link no clone could follow. So the names are git's, and
# so are the folders.
declare -A tracked=()
while IFS= read -r -d '' path; do
    tracked["$path"]=1
    while [[ "$path" == */* ]]; do
        path="${path%/*}"
        [[ -n "${tracked[$path]:-}" ]] && break
        tracked["$path"]=1
    done
done < <(git ls-files -z)
resolves() { # a path from the repository's root, with `.` and `..` still in it
    local part
    local -a parts=() kept=()
    IFS='/' read -r -a parts <<<"$1"
    for part in "${parts[@]}"; do
        case "$part" in
        '' | .) ;;
        ..)
            [[ ${#kept[@]} -eq 0 ]] && return 1
            unset 'kept[${#kept[@]}-1]'
            ;;
        *) kept+=("$part") ;;
        esac
    done
    [[ ${#kept[@]} -eq 0 ]] && return 0
    local joined
    joined="$(IFS=/; echo "${kept[*]}")"
    [[ -n "${tracked[$joined]:-}" ]]
}
while IFS=: read -r file link; do
    target="${link%%#*}"
    [[ -z "$target" ]] && continue
    base="$(dirname "$file")"
    if ! resolves "$base/$target" && ! resolves "$target"; then
        err "broken relative link: $link" "$file"
        status=1
    fi
# Tracked files only, for the reason the legal sweep below gives in full: a
# working tree also holds what this repository does not publish -- a scratch
# note, a draft, a folder somebody is still assembling -- and sweeping those
# makes the gate depend on whose machine it runs on. It failed on exactly
# that: an untracked `art/` directory with a half-written relative path in it.
# The legal sweep learned this and this check had not.
done < <(git ls-files -z -- '*.md' ':(exclude)third_party' \
    | xargs -0 grep -noE '\]\(([^)#h][^):]*)\)' 2>/dev/null |
    sed -E 's/^([^:]+):[0-9]+:\]\(([^)]*)\)/\1:\2/')

# --- Pinned versions are named identically everywhere -----------------------
echo "== version consistency =="
check_version() { # expected, forbidden-pattern, label
    # Tracked files, as every check here that is about this repository: what
    # sits untracked in a checkout is not what a clone has.
    if git ls-files -z -- '*.md' '*.toml' '*.json' ':(exclude)third_party' |
        xargs -0 grep -nE "$2" 2>/dev/null; then
        err "version inconsistency for $3 (found '$2'; expected only '$1')"
        status=1
    fi
}
check_version "0.734" "pinned to 0\.7(0|1|2)[0-9]" "Luau"
check_version "lute@1.0.0" "lute@0\." "Lute"
check_version "luau-lsp@1.69.0" "luau-lsp@1\.6[0-8]\." "luau-lsp"

# --- Legal sweep (R7) -------------------------------------------------------
# **The name is assembled rather than written**, so this file -- which is code
# -- does not spell the thing it forbids. The rule is that no corner of the
# code refers to that platform, and the gate enforcing it is not an exception
# to it.
#
# What may name it is DOCUMENTATION a person reads to decide whether this engine
# is for them: the migration guide and the design notes that explain a
# divergence. Nothing
# executable, nothing generated from code, and no configuration file is on that
# list any more -- the generators emit the migration section under a neutral
# slug, and the editor settings explain themselves without naming anybody.
echo "== legal sweep (R7) =="
vendor="$(printf '%s%s' 'rob' 'lox')"
allowed='^(\./)?(README\.md|CONTRIBUTING\.md|NOTICE|docs/|templates/README\.md|examples/README\.md|tests/README\.md|runtime/README\.md|api/README\.md|engine/README\.md|tools/README\.md|third_party/README\.md|branding/README\.md|art/)'
while IFS= read -r f; do
    if ! [[ "$f" =~ $allowed ]]; then
        err "the platform R7 forbids is named outside the allowed docs set" "$f"
        status=1
    fi
    # Tracked files only. R7 is about what this repository publishes, and a
    # working tree also holds things it does not: a developer's local editor
    # state, a scratch file, an ignored tool config. Sweeping those made the
    # gate depend on whose machine it ran on: an ignored tool configuration
    # turned it red on one.
done < <(git ls-files -z -- . ':(exclude)third_party' ':(exclude).github' \
    | xargs -0 grep -liE "$vendor" 2>/dev/null || true)

# **The indirect names, in code.** A type or class name that exists only on that
# platform, its editor's product name standing alone, or its unit of length, is
# the same reference
# spelled another way -- and one of each had survived the sweep above because it
# never contained the word. Code only: the documentation set may still explain
# a divergence by naming what it diverges from. "Visual Studio" and "Android
# Studio" are toolchains, not the reference, and are excluded by name.
echo "== legal sweep (R7, indirect names in code) =="
indirect="$(printf '%s|%s|%s|%s' 'RBX[A-Z]' 'Bindable(Event|Function)' '(^|[^A-Za-z])Studio([^A-Za-z]|$)'     '(^|[^A-Za-z])[Ss]tuds?([^A-Za-z]|$)')"
#
# **A scene's terrain and blocks are bytes, not names.** They travel as base64
# inside the JSON, and a long enough run of base64 spells anything: the
# flagship's sculpted ground contains `RBX` followed by a capital letter
# twenty-nine times, none of them a word. So a `.json` file is swept with those
# two payloads blanked out first, and everything else in it -- every name,
# every property, every class -- is swept exactly as code is.
codeFiles() {
    git ls-files -z -- engine runtime shaders tools api tests examples templates i18n cmake scripts \
        ':(exclude)*.md' ':(exclude)*.png' ':(exclude)scripts/gates/docs-lint.sh'
}
while IFS= read -r hit; do
    case "$hit" in
    *"Visual Studio"* | *"Android Studio"*) continue ;;
    esac
    err "an indirect reference R7 forbids: ${hit#*:*:}" "${hit%%:*}"
    status=1
done < <(
    codeFiles | grep -zv '\.json$' | xargs -0 grep -nE "$indirect" 2>/dev/null || true
    codeFiles | grep -z '\.json$' | xargs -0 awk -v re="$indirect" '{
        line = $0
        gsub(/"(terrain|blocks)": *"[A-Za-z0-9+\/=]*"/, "\"payload\": \"\"", line)
        if (line ~ re) print FILENAME ":" FNR ":" line
    }' 2>/dev/null || true
)

# --- Every example carries its launcher (examples/README.md) ----------------
#
# The README states this as a convention and a convention in prose is a
# convention the next example forgets -- which is exactly what happened when
# `02-meshes` shipped without one. A launcher nothing depends on still has to
# exist, because its whole job is that a human does not have to remember where
# an out-of-tree build put the binary (R14).
echo "== every example has a launcher =="
for example in examples/*/; do
    [[ -f "$example/README.md" || -d "$example/src" || -f "$example/init.luau" ]] || continue
    if [[ ! -f "$example/run.bat" ]]; then
        err "$example has no run.bat (examples/README.md: every example folder carries one)"
        status=1
    fi
done

# **Text read as one encoding and written as another.** A tool that reads a
# UTF-8 file as the system's code page and writes it back as UTF-8 turns every
# character past ASCII into two: a section sign into an A with a circumflex and
# a section sign, a dash into three letters. It compiles, it runs, and 24
# comments in `engine.cpp` said it for two commits before anybody read one.
# The bytes are named in hex so that this file does not contain what it looks
# for, and matched as fixed strings in the C locale: `grep -P` refuses the
# locale Git Bash runs in on Windows, and with its complaint thrown away the
# first version of this check passed a file it was written to catch.
echo "== encoding (no text encoded twice) =="
twice="$(git ls-files -z -- engine runtime tools api templates examples scripts cmake tests platforms i18n docs \
        CMakeLists.txt CHANGELOG.md \
    | LC_ALL=C xargs -0 grep -I -l -F -e $'\xc3\x82\xc2' -e $'\xc3\xa2\xe2\x82\xac' -e $'\xc3\x83\xc2' || true)"
if [[ -n "$twice" ]]; then
    printf '%s\n' "$twice" | head -50 | while IFS= read -r file; do
        err "has text that was encoded twice (read as a code page, written as UTF-8); write the characters again" "$file"
    done
    status=1
fi

# **Checks a checkout adds of its own.** When `ENG_EXTRA_CHECKS` names a
# directory, every `*.sh` in it is run from the repository's root, in name
# order, and one that fails fails this gate. Unset, nothing is run and nothing
# is said. Set to something that is not a directory, the gate fails: a path
# mistyped would otherwise be checks silently not run. A Windows path's
# backslashes are read as slashes.
if [[ -n "${ENG_EXTRA_CHECKS:-}" ]]; then
    extras="${ENG_EXTRA_CHECKS//\\//}"
    if [[ -d "$extras" ]]; then
        for extra in "$extras"/*.sh; do
            [[ -f "$extra" ]] || continue
            if ! bash "$extra"; then
                status=1
            fi
        done
    else
        echo "docs-lint: ENG_EXTRA_CHECKS names '$ENG_EXTRA_CHECKS', which is not a directory" >&2
        status=1
    fi
fi

if [[ $status -eq 0 ]]; then
    echo "docs-lint: ok"
fi
exit $status
