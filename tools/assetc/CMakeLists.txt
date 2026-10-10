# tools/assetc -- the offline asset compiler (roadmap M7, M7 brief Decision 1).
#
# Unlike tools/imgcmp, this one DOES link engine modules, and that is the point:
# `asset::encodeMesh` and `asset::decodeMesh` are one implementation shared by
# the tool that writes a pack and the runtime that reads it. Two would drift,
# and the milestone that discovered they had drifted would discover it as a
# world that renders wrong.
#
# The basis ENCODER is linked here and nowhere else. `engine/asset` links only
# the transcoder, so "the engine cannot write a texture" is a property of the
# build rather than a promise.

add_library(engine_assetc_lib STATIC
    src/compiler.cpp
    src/texture.cpp
    src/exotic.cpp
    src/l10n.cpp
)
target_include_directories(engine_assetc_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(engine_assetc_lib
    PUBLIC engine::asset engine::core
    # `jobs` for the parallel texture encode (E9 step 11). Its determinism rule
    # is what makes it usable here at all: per-job buffers, a barrier, and a
    # merge in source order, so the pack is built from the same bytes in the same
    # sequence however many workers ran.
    #
    # `audio` for one function (ADR 0200 §2): `audio::detail::probeFrames`, how
    # long a sound file says it is. The localization index holds the number the
    # engine reads from the same file in a dev run, so it is the engine's own
    # measure that is called and not a second one written here. A downward
    # edge: `audio` is at 4 and this is at 5.
    PRIVATE engine::warnings engine::platform engine::jobs engine::audio basis::encoder)

# assimp, and ONLY here (ADR 0010). The engine reads `.lmesh`, a format assimp
# has never heard of, and the layering gate reading real `#include` edges is what
# keeps that true rather than a comment.
#
# The definition is always set so `exotic.cpp` compiles either way: with the
# importer off it becomes a keyed error naming the extension, which is a better
# answer than a link failure or a model silently treated as an opaque blob.
if(ENG_ASSETC_ASSIMP)
    target_compile_definitions(engine_assetc_lib PRIVATE ENG_ASSETC_ASSIMP=1)
    target_link_libraries(engine_assetc_lib PRIVATE assimp::assimp)
else()
    target_compile_definitions(engine_assetc_lib PRIVATE ENG_ASSETC_ASSIMP=0)
endif()

add_executable(assetc src/main.cpp)
target_link_libraries(assetc PRIVATE engine::warnings engine_assetc_lib engine::platform engine::jobs)
# Where a surface shader's compiler and the engine's headers are in a build
# tree (ADR 0091); a packaged editor carries both beside the binary instead.
if(ENG_SHADER_TOOLCHAIN)
    target_compile_definitions(assetc PRIVATE
        ENG_DEV_SHADERCROSS="${CMAKE_BINARY_DIR}/host-tools/shadercross${CMAKE_EXECUTABLE_SUFFIX}")
else()
    target_compile_definitions(assetc PRIVATE ENG_DEV_SHADERCROSS="")
endif()
target_compile_definitions(assetc PRIVATE ENG_DEV_SURFACE_INCLUDE="${CMAKE_SOURCE_DIR}/shaders/include")

# The catalog beside the binary, for the same reason `engine-host` gets one: an
# engine error this tool relays should read as prose rather than as a bare key.
#
# **A target that runs every build, not a post-build step.** A post-build step
# runs when the binary relinks, and a change to the catalog alone relinks
# nothing -- so the staged copy went stale, and the packager then laid this
# stale copy over the host's fresh one: a message added in the same commit
# printed as `[i18n:missing]` from the packaged build. `copy_if_different` makes
# running it every time cost a comparison.
add_custom_target(assetc_catalog ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/i18n/en.json"
        "${CMAKE_CURRENT_BINARY_DIR}/content/i18n/en.json"
    COMMENT "Staging the engine message catalog for assetc")
add_dependencies(assetc assetc_catalog)

if(ENG_BUILD_TESTS)
    add_executable(engine_assetc_tests
        tests/compiler_tests.cpp
        tests/texture_tests.cpp
        tests/l10n_tests.cpp
    )
    target_link_libraries(engine_assetc_tests PRIVATE
        engine::warnings
        doctest::doctest_with_main
        engine_assetc_lib
        engine::platform
        # The localization index is held to the engine's own measure of a
        # sound file, so its tests ask the engine.
        engine::audio
    )
    target_compile_definitions(engine_assetc_tests PRIVATE
        ENG_TEST_CATALOG="${CMAKE_SOURCE_DIR}/i18n/en.json"
        ENG_ASSET_TEST_DATA="${CMAKE_SOURCE_DIR}/engine/asset/tests/data"
        # Real Ogg Vorbis, borrowed from `audio` rather than copied.
        ENG_AUDIO_TEST_DATA="${CMAKE_SOURCE_DIR}/engine/audio/tests/data"
        ENG_ASSETC_ASSIMP=$<BOOL:${ENG_ASSETC_ASSIMP}>)
    add_test(NAME assetc COMMAND engine_assetc_tests)
endif()
