# L5 -- the game VM and every binding into it (architecture.md §2, §5).
#
# This is the ONLY module allowed to include a Luau header. Rule 2 of the
# layering says so, and the reason is not tidiness: `core` mirrors the `vector`
# memory layout without ever including `lua.h`, which is what lets tooling and
# tests use the engine's types with no VM at all. The layer checker enforces the
# direction; this CMakeLists is where the dependency is allowed to exist.
#
# It links `scene` and turns that module's POD change facts into signal fires.
# `scene` cannot do that itself and must not be able to: signal arguments are
# arbitrary script values, so a scene that owned the queue would hold references
# into the VM.

engine_add_module(script
    LAYER 5
    SOURCES
        src/animation.cpp
        src/builtins.cpp
        src/bytecode.cpp
        src/input_events.cpp
        src/binding.cpp
        src/datatypes.cpp
        src/instance_binding.cpp
        src/materials.cpp
        src/modules.cpp
        src/debugger.cpp
        src/net_module.cpp
        src/print_tree.cpp
        src/save_store.cpp
        src/save_service.cpp
        src/services.cpp
        src/remote.cpp
        src/signals.cpp
        src/tasks.cpp
        src/tweens.cpp
        src/sandbox.cpp
        src/sequences.cpp
        src/stdlib.cpp
        src/reload_state.cpp
        src/runtime.cpp
    DEPS
        audio
        core
        input
        nav
        net_api
        physics_api
        platform
        scene
)

target_link_libraries(engine_script PRIVATE engine::luau)
# xxh3, for a save file's checksum (ADR 0111), header-only and private, as the
# world hash uses it.
target_include_directories(engine_script SYSTEM PRIVATE "${ENG_THIRD_PARTY_DIR}/xxhash")

# `render` is linked into the TEST executable and nowhere else, the same
# arrangement `render`'s own tests use for `rhi_null` and for the same reason: a
# test binary is not a module. `script` binds `AnimationPlayer:LoadAnimation`,
# and the boot-time cross-check counts a binding whose class nothing declared as
# a mismatch -- so the fixture has to be able to register render's classes. The
# module itself still sees only `scene::AnimationHost`.
engine_add_module_tests(script
    MODULES
        script
        render
    SOURCES
        tests/sandbox_tests.cpp
        tests/datatype_tests.cpp
        tests/instance_binding_tests.cpp
        tests/service_tests.cpp
        tests/signal_tests.cpp
        tests/task_tests.cpp
        tests/debugger_tests.cpp
        tests/print_tests.cpp
        tests/save_tests.cpp
        tests/bytecode_tests.cpp
)

# The sandbox tests boot a VM directly rather than through `ScriptRuntime`, so
# that a sandbox failure has exactly one possible cause. That means they need
# Luau too.
#
# The catalog is a real dependency of the binding tests rather than a
# convenience: an error is identified by its `[key]` prefix, and the prefix is
# the catalog's own name for the key (ADR 0019). Without it every raise reports
# `[i18n:missing:xxxxxxxx]` and a test asserting on a key would pass only by
# matching nothing.
if(ENG_BUILD_TESTS)
    target_link_libraries(engine_script_tests PRIVATE engine::luau)
    target_compile_definitions(engine_script_tests PRIVATE
        ENG_TEST_CATALOG="${CMAKE_SOURCE_DIR}/i18n/en.json")
endif()

# **The builtins written in Luau, compiled at build time** (ADR 0094). The
# shipping profile has no compiler, so `Collector` and `Promise` ship as
# bytecode, compiled by a host tool with the runtime's own options
# (`compile_options.h`) and embedded in this module. In install order.
#
# **Cross-compiling, the tool has to be the build machine's**: an arm64 build
# of it cannot run on the PC that is building for a phone. Such a build names
# one built for the host (`ENG_HOST_LUAUEMBED`), as it names shadercross.
if(CMAKE_CROSSCOMPILING)
    set(ENG_HOST_LUAUEMBED "" CACHE FILEPATH
        "engine_luauembed built for the build machine, for a cross-compiled build")
    if(ENG_HOST_LUAUEMBED)
        set(ENG_LUAUEMBED_COMMAND "${ENG_HOST_LUAUEMBED}")
        set(ENG_LUAUEMBED_DEPENDS "${ENG_HOST_LUAUEMBED}")
    else()
        # Not a configure error: a cross build of a sample that never links
        # this module (the nightly's triangle) needs no tool. One that does
        # fails here, saying what to pass.
        set(ENG_LUAUEMBED_COMMAND ${CMAKE_COMMAND} -E echo
            "A cross-compiled build needs -DENG_HOST_LUAUEMBED=<engine_luauembed built for this machine>"
            COMMAND ${CMAKE_COMMAND} -E false)
        set(ENG_LUAUEMBED_DEPENDS "")
    endif()
else()
    add_executable(engine_luauembed tools/luauembed.cpp)
    target_include_directories(engine_luauembed PRIVATE include)
    target_link_libraries(engine_luauembed PRIVATE engine::warnings Luau.Compiler)
    set(ENG_LUAUEMBED_COMMAND engine_luauembed)
    set(ENG_LUAUEMBED_DEPENDS engine_luauembed)
endif()

set(ENG_BUILTINS
    Promise=${CMAKE_SOURCE_DIR}/runtime/builtins/promise.luau
    Collector=${CMAKE_SOURCE_DIR}/runtime/builtins/collector.luau)
set(ENG_BUILTIN_FILES
    ${CMAKE_SOURCE_DIR}/runtime/builtins/promise.luau
    ${CMAKE_SOURCE_DIR}/runtime/builtins/collector.luau)
set(ENG_BUILTINS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/builtins.gen.cpp)
add_custom_command(
    OUTPUT ${ENG_BUILTINS_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${ENG_LUAUEMBED_COMMAND} ${ENG_BUILTINS_GENERATED} ${ENG_BUILTINS}
    DEPENDS ${ENG_LUAUEMBED_DEPENDS} ${ENG_BUILTIN_FILES}
    COMMENT "Compiling the builtins written in Luau"
    VERBATIM)
target_sources(engine_script PRIVATE ${ENG_BUILTINS_GENERATED})
