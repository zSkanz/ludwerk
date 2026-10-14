# Module declaration helper with mechanical layer enforcement
# (architecture.md §2).
#
# The layering is the engine's main defence against turning into a ball of mud,
# so it is enforced at declaration time here (a module may only name deps at
# strictly lower layers) and again in CI by tools/repo/checklayers.luau, which
# reads the actual #include lines. Both are needed: this catches a wrong
# link-time dependency, the include checker catches a header sneaking across a
# seam without a link edge.
#
#   L0 core
#   L1 jobs, platform
#   L2 rhi_api, physics_api, net_api, asset
#   L3 scene
#   L4 render, audio, input, nav
#   L5 ui, script
#   L6 app

define_property(GLOBAL PROPERTY ENG_MODULES
    BRIEF_DOCS "All declared engine modules" FULL_DOCS "All declared engine modules")

function(engine_add_module name)
    cmake_parse_arguments(ARG "INTERFACE" "LAYER;INCLUDE_DIR" "SOURCES;DEPS;API_DEPS;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})

    if(NOT DEFINED ARG_LAYER)
        message(FATAL_ERROR "engine_add_module(${name}): LAYER is required")
    endif()
    if(ARG_INTERFACE AND ARG_SOURCES)
        message(FATAL_ERROR "engine_add_module(${name}): an INTERFACE module has no SOURCES")
    endif()
    if(NOT ARG_INTERFACE AND NOT ARG_SOURCES)
        message(FATAL_ERROR "engine_add_module(${name}): SOURCES is required")
    endif()

    # Several targets can share one directory: the *_api seam and its backends
    # live together (architecture.md's tree has one `rhi/` folder, not four), so
    # the include root is a parameter rather than always ./include.
    if(NOT DEFINED ARG_INCLUDE_DIR)
        set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
    endif()

    set(target "engine_${name}")

    if(ARG_INTERFACE)
        add_library(${target} INTERFACE)
        set(scope INTERFACE)
    else()
        add_library(${target} STATIC ${ARG_SOURCES})
        set(scope PUBLIC)
    endif()
    add_library(engine::${name} ALIAS ${target})

    set_property(GLOBAL APPEND PROPERTY ENG_MODULES ${name})
    set_property(GLOBAL PROPERTY ENG_MODULE_LAYER_${name} ${ARG_LAYER})
    set_property(GLOBAL PROPERTY ENG_MODULE_INTERFACE_${name} ${ARG_INTERFACE})

    target_include_directories(${target} ${scope} $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>)

    # Every module we author is held to the full warning bar; vendored code is
    # exempt by being added SYSTEM. An INTERFACE target compiles nothing, so it
    # has nothing to hold to it.
    if(NOT ARG_INTERFACE)
        target_link_libraries(${target} PRIVATE engine::warnings)
    endif()

    foreach(dep IN LISTS ARG_DEPS)
        get_property(dep_layer GLOBAL PROPERTY ENG_MODULE_LAYER_${dep})
        if(dep_layer STREQUAL "")
            message(FATAL_ERROR
                "engine_add_module(${name}): depends on '${dep}', which is not declared yet.\n"
                "Modules must be added in layer order in the root CMakeLists.")
        endif()
        if(NOT dep_layer LESS ARG_LAYER)
            message(FATAL_ERROR
                "Layer violation: ${name} (L${ARG_LAYER}) may not depend on ${dep} (L${dep_layer}).\n"
                "A module may only depend on strictly lower layers (architecture.md §2).\n"
                "If ${dep} is this module's own interface seam, use API_DEPS.")
        endif()
        target_link_libraries(${target} ${scope} engine::${dep})
    endforeach()

    # The one sanctioned same-layer edge: a backend implements the seam it sits
    # behind, and architecture.md §2 puts both at the same layer on purpose --
    # everything above sees only the seam. Kept separate from DEPS so that
    # "same layer" stays an error everywhere else, and narrowed to interface
    # targets so it cannot become a general escape hatch.
    foreach(dep IN LISTS ARG_API_DEPS)
        get_property(dep_layer GLOBAL PROPERTY ENG_MODULE_LAYER_${dep})
        get_property(dep_interface GLOBAL PROPERTY ENG_MODULE_INTERFACE_${dep})
        if(dep_layer STREQUAL "")
            message(FATAL_ERROR "engine_add_module(${name}): API_DEPS names '${dep}', which is not declared yet.")
        endif()
        if(NOT dep_layer EQUAL ARG_LAYER)
            message(FATAL_ERROR
                "engine_add_module(${name}): API_DEPS is for a seam at the SAME layer; "
                "${dep} is L${dep_layer} and ${name} is L${ARG_LAYER}. Use DEPS.")
        endif()
        if(NOT dep_interface)
            message(FATAL_ERROR
                "engine_add_module(${name}): API_DEPS requires ${dep} to be an INTERFACE module.")
        endif()
        target_link_libraries(${target} ${scope} engine::${dep})
    endforeach()

    if(ARG_PUBLIC_DEPS)
        target_link_libraries(${target} ${scope} ${ARG_PUBLIC_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
        if(ARG_INTERFACE)
            message(FATAL_ERROR "engine_add_module(${name}): an INTERFACE module has no PRIVATE_DEPS")
        endif()
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
endfunction()

# One test executable per module directory (architecture.md §9), registered with
# ctest under `name`. MODULES lists the Ludwerk targets under test when the
# directory holds more than one -- a seam and its backends, say -- and defaults
# to `name` itself.
#
# **Shards** (ADR 0148): an executable whose cases take a minute between them
# is the long pole of a parallel ctest. `SHARD_CASES "suffix|pattern"` runs the
# cases matching a doctest pattern as `name_suffix`, and `SHARD_FILES
# "suffix|pattern"` the cases of the source files matching one; `name` runs
# what is left. Every case runs once, in exactly one of them.
function(engine_add_module_tests name)
    if(NOT ENG_BUILD_TESTS)
        return()
    endif()

    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;MODULES;SHARD_CASES;SHARD_FILES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "engine_add_module_tests(${name}): SOURCES is required")
    endif()
    if(NOT ARG_MODULES)
        set(ARG_MODULES ${name})
    endif()

    set(target "engine_${name}_tests")
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE
        engine::warnings
        doctest::doctest_with_main
        ${ARG_DEPS})

    # Test-only helpers shared across modules. Headers only, and deliberately
    # not a module: nothing in `engine/` may depend on it, and it must not be
    # able to acquire a link-time half.
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/tests/support)

    foreach(module IN LISTS ARG_MODULES)
        target_link_libraries(${target} PRIVATE engine::${module})
    endforeach()

    set(cases "")
    foreach(shard IN LISTS ARG_SHARD_CASES)
        string(REPLACE "|" ";" parts "${shard}")
        list(GET parts 0 suffix)
        list(GET parts 1 pattern)
        add_test(NAME ${name}_${suffix} COMMAND ${target} "--test-case=${pattern}")
        list(APPEND cases "${pattern}")
    endforeach()
    string(JOIN "," caseFilter ${cases})
    set(files "")
    foreach(shard IN LISTS ARG_SHARD_FILES)
        string(REPLACE "|" ";" parts "${shard}")
        list(GET parts 0 suffix)
        list(GET parts 1 pattern)
        set(arguments "--source-file=${pattern}")
        if(caseFilter)
            list(APPEND arguments "--test-case-exclude=${caseFilter}")
        endif()
        add_test(NAME ${name}_${suffix} COMMAND ${target} ${arguments})
        list(APPEND files "${pattern}")
    endforeach()
    string(JOIN "," fileFilter ${files})
    set(rest "")
    if(caseFilter)
        list(APPEND rest "--test-case-exclude=${caseFilter}")
    endif()
    if(fileFilter)
        list(APPEND rest "--source-file-exclude=${fileFilter}")
    endif()
    add_test(NAME ${name} COMMAND ${target} ${rest})
endfunction()
