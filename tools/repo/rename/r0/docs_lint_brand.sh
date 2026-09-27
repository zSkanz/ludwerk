# **The brand lives in branding/brand.toml and nowhere else in code** (ADR
# 0109). The old name in code, the build or a project is a rename left undone;
# the brand name itself in code is a rebrand waiting to happen. A REPORT until
# the rename's stage R5 turns it into a failure (docs/briefs/rename-kickoff.md):
# before that, every stage shrinks this list and the list is the progress.
# `BRAND_LINT=fail` makes it fail now, for trying a stage out.
echo "== brand (ADR 0109) =="
brandHits="$(git ls-files -z -- engine runtime tools api templates examples scripts cmake samples tests platforms \
        CMakeLists.txt CMakePresets.json \
        ':(exclude)*.md' ':(exclude)*.png' ':(exclude)*.ico' ':(exclude)scripts/gates/docs-lint.sh' \
    | xargs -0 grep -I -l -E 'luaug|LuauG|LUAUG' 2>/dev/null | sort -u || true)"
if [[ -n "$brandHits" ]]; then
    brandCount="$(printf '%s\n' "$brandHits" | wc -l | tr -d ' ')"
    if [[ "${BRAND_LINT:-report}" == "fail" ]]; then
        printf '%s\n' "$brandHits" | head -50 | while IFS= read -r file; do
            err "names the old brand; the code says engine and reads branding/brand.toml (ADR 0109)" "$file"
        done
        status=1
    else
        echo "  $brandCount file(s) still name the old brand (a report until rename stage R5)"
    fi
fi

