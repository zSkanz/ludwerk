# Build provenance (ADR 0031).
#
# third_party/manifest.json is the authority on what is pinned (ADR 0021). This
# reads the Luau row at configure time and generates a header carrying the
# version and full commit SHA, so the value `--version` prints is derived from
# the pin rather than duplicated in source where the two could drift.
#
# Luau ships no version constant of its own -- verified across the whole
# vendored tree at 0.734 -- which is why the manifest is the source here and
# why the gate additionally checks header-derived ABI constants (see
# engine/app: those come from the vendored headers at compile time).

set(_engine_manifest "${ENG_THIRD_PARTY_DIR}/manifest.json")

if(NOT EXISTS "${_engine_manifest}")
    message(FATAL_ERROR "third_party/manifest.json not found -- run: lute tools/repo/vendor.luau sync")
endif()

# Re-configure when the pin changes, so a manifest edit can never leave a stale
# provenance header behind.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_manifest}")

file(READ "${_engine_manifest}" _engine_manifest_text)

# Manifest rows are one JSON object per line, so the row is matched first and
# its fields second -- this cannot accidentally pick up another dependency's
# version the way a whole-file field match could.
string(REGEX MATCH "\"name\"[ \t]*:[ \t]*\"luau\"[^\n]*" _engine_luau_row "${_engine_manifest_text}")
if(_engine_luau_row STREQUAL "")
    message(FATAL_ERROR "third_party/manifest.json has no dependency row named \"luau\"")
endif()

string(REGEX MATCH "\"version\"[ \t]*:[ \t]*\"([^\"]+)\"" _engine_unused "${_engine_luau_row}")
set(ENG_LUAU_VERSION "${CMAKE_MATCH_1}")

string(REGEX MATCH "\"commit\"[ \t]*:[ \t]*\"([^\"]+)\"" _engine_unused "${_engine_luau_row}")
set(ENG_LUAU_COMMIT "${CMAKE_MATCH_1}")

# CMake's regex engine has no bounded-repetition operator, so the length is
# checked separately rather than with {40}.
string(LENGTH "${ENG_LUAU_COMMIT}" _engine_commit_length)
if(NOT _engine_commit_length EQUAL 40 OR NOT ENG_LUAU_COMMIT MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "Luau is not pinned to a resolved commit (manifest says '${ENG_LUAU_COMMIT}').\n"
        "Resolve it with: lute tools/repo/vendor.luau resolve luau ${ENG_LUAU_VERSION}")
endif()

if(NOT EXISTS "${ENG_THIRD_PARTY_DIR}/luau/VM/include/lua.h")
    message(FATAL_ERROR
        "Luau is pinned but not vendored -- run: lute tools/repo/vendor.luau sync luau")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/build_info.h.in"
    "${ENG_GENERATED_DIR}/engine/core/build_info.h"
    @ONLY)

# **The brand (ADR 0109)**: branding/brand.toml, one `key = "value"` per line.
# Read here, once, into a generated header, so that no C++ source spells the
# product's name.
set(_engine_brand_file "${CMAKE_SOURCE_DIR}/branding/brand.toml")
if(NOT EXISTS "${_engine_brand_file}")
    message(FATAL_ERROR "branding/brand.toml not found: the product's name lives there (ADR 0109)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_brand_file}")
file(STRINGS "${_engine_brand_file}" _engine_brand_lines)
foreach(_engine_brand_key IN ITEMS name short tagline domain previous)
    set(_engine_brand_value "")
    foreach(_engine_brand_line IN LISTS _engine_brand_lines)
        if(_engine_brand_line MATCHES "^${_engine_brand_key}[ \t]*=[ \t]*\"([^\"]*)\"")
            set(_engine_brand_value "${CMAKE_MATCH_1}")
        elseif(_engine_brand_line MATCHES "^${_engine_brand_key}[ \t]*=[ \t]*\\[(.*)\\]")
            # An array of strings (`previous = ["Old", "Older"]`): its items, comma-separated.
            string(REGEX REPLACE "[\" \t]" "" _engine_brand_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    string(TOUPPER "${_engine_brand_key}" _engine_brand_upper)
    set(ENG_BRAND_${_engine_brand_upper} "${_engine_brand_value}")
endforeach()
if(ENG_BRAND_NAME STREQUAL "" OR NOT ENG_BRAND_SHORT MATCHES "^[a-z][a-z0-9-]*$")
    message(FATAL_ERROR
        "branding/brand.toml needs a name and a short name of lowercase letters, digits and dashes "
        "(name = '${ENG_BRAND_NAME}', short = '${ENG_BRAND_SHORT}')")
endif()
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/brand.h.in"
    "${ENG_GENERATED_DIR}/engine/core/brand.h"
    @ONLY)

add_library(engine_build_info INTERFACE)
add_library(engine::build_info ALIAS engine_build_info)
target_include_directories(engine_build_info INTERFACE "${ENG_GENERATED_DIR}")

message(STATUS "${ENG_BRAND_NAME}: Luau ${ENG_LUAU_VERSION} @ ${ENG_LUAU_COMMIT}")
