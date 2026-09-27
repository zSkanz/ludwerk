# **The brand lives in branding/brand.toml and nowhere else in code** (ADR
# 0109). The old name in code, the build, a project or a script is a rename
# left undone. The exceptions: the brand file itself (its record of the names
# before), the rename's own tools, and `luaug_dpow` -- a C symbol the vendored,
# patched Luau calls, which renaming would mean re-vendoring.
echo "== brand (ADR 0109) =="
brandHits="$(git ls-files -z -- engine runtime tools api templates examples scripts cmake samples tests platforms \
        shaders branding .github CMakeLists.txt CMakePresets.json \
        third_party/CMakeLists.txt third_party/manifest.json third_party/README.md \
        ':(exclude)*.md' ':(exclude)*.png' ':(exclude)*.ico' ':(exclude)scripts/gates/docs-lint.sh' \
        ':(exclude)branding/brand.toml' ':(exclude)tools/repo/rename' \
    | xargs -0 grep -I -n -E 'luaug|LuauG|LUAUG' 2>/dev/null | grep -v 'luaug_dpow' | cut -d: -f1 | sort -u || true)"
if [[ -n "$brandHits" ]]; then
    printf '%s\n' "$brandHits" | head -50 | while IFS= read -r file; do
        err "names the old brand; the code says engine and reads branding/brand.toml (ADR 0109)" "$file"
    done
    status=1
fi

