# **The brand (ADR 0109)**: branding/brand.toml, one `key = "value"` per line.
# Read here, once, into a generated header, so that no C++ source spells the
# product's name.
set(_luaug_brand_file "${CMAKE_SOURCE_DIR}/branding/brand.toml")
if(NOT EXISTS "${_luaug_brand_file}")
    message(FATAL_ERROR "branding/brand.toml not found: the product's name lives there (ADR 0109)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_luaug_brand_file}")
file(STRINGS "${_luaug_brand_file}" _luaug_brand_lines)
foreach(_luaug_brand_key IN ITEMS name short tagline domain previous)
    set(_luaug_brand_value "")
    foreach(_luaug_brand_line IN LISTS _luaug_brand_lines)
        if(_luaug_brand_line MATCHES "^${_luaug_brand_key}[ \t]*=[ \t]*\"([^\"]*)\"")
            set(_luaug_brand_value "${CMAKE_MATCH_1}")
        elseif(_luaug_brand_line MATCHES "^${_luaug_brand_key}[ \t]*=[ \t]*\\[(.*)\\]")
            # An array of strings (`previous = ["LuauG"]`): its items, comma-separated.
            string(REGEX REPLACE "[\" \t]" "" _luaug_brand_value "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    string(TOUPPER "${_luaug_brand_key}" _luaug_brand_upper)
    set(LUAUG_BRAND_${_luaug_brand_upper} "${_luaug_brand_value}")
endforeach()
if(LUAUG_BRAND_NAME STREQUAL "" OR NOT LUAUG_BRAND_SHORT MATCHES "^[a-z][a-z0-9-]*$")
    message(FATAL_ERROR
        "branding/brand.toml needs a name and a short name of lowercase letters, digits and dashes "
        "(name = '${LUAUG_BRAND_NAME}', short = '${LUAUG_BRAND_SHORT}')")
endif()
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/brand.h.in"
    "${LUAUG_GENERATED_DIR}/luaug/core/brand.h"
    @ONLY)

