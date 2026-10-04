# L4 -- the mixer and the sound timeline (architecture.md §2 `audio`, ADR 0009).
#
# **At L4 rather than L2, from M6.** The sketch put it beside the other backend
# seams, and it owns the `Sound` and `AudioGroup` classes -- a module that
# registers a class into scene's registry has to be able to see scene, and L2
# cannot. ADR 0009's seam is untouched by the move: the seam is the module
# boundary, not its layer, and `render` sits here for exactly the same reason.
#
# miniaudio is compiled in ONE translation unit and appears in no header. That is
# what makes "the public API never leaks miniaudio concepts" structural: nothing
# outside src/audio.cpp can name a miniaudio type even by accident.
#
# `src/vorbis.cpp` is the Vorbis decoder miniaudio ships beside itself and
# decodes through (D546), in a translation unit of its own.

engine_add_module(audio
    LAYER 4
    SOURCES
        src/audio.cpp
        src/native_accessors.cpp
        src/vorbis.cpp
        generated/class_descriptors.gen.cpp
    DEPS
        asset
        core
        scene
        # Both from D129's prefetch: the read is `platform::readFileAsync` and
        # the decode is a job. Named here rather than leaned on transitively,
        # because `tools/repo/checklayers.luau` reads real `#include` edges and
        # an unnamed one is an edge nobody decided.
        jobs
        platform
)

target_include_directories(engine_audio SYSTEM PRIVATE "${ENG_THIRD_PARTY_DIR}/miniaudio")

engine_add_module_tests(audio
    MODULES
        audio
    SOURCES
        tests/audio_tests.cpp
)
# Real Ogg Vorbis files (D546): tones a libvorbis encoder wrote, since the
# engine has none to write them with at test time.
if(TARGET engine_audio_tests)
    target_compile_definitions(engine_audio_tests PRIVATE
        ENG_AUDIO_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
