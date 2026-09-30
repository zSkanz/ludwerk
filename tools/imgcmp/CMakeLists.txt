# tools/imgcmp -- the screenshot tolerance comparator named by the M1 gate
# (roadmap.md M1; architecture.md §9 "render tests").
#
# A tool, not an engine module: it uses plain add_executable/add_library rather
# than engine_add_module(), and links nothing from engine/. That independence is
# the point -- this is how the agent verifies a rendered frame, so a broken
# engine build must not also take away its eyes.

# ---------------------------------------------------------------------------
# stb (manifest.json row "stb", MIT OR Public Domain).
#
# Header-only, and imgcmp is its only consumer today, so it is wired here rather
# than in third_party/CMakeLists.txt -- which by its own rule adds a dependency
# only once a module links it. Move it there when a second target needs stb.
# ---------------------------------------------------------------------------
add_library(engine_imgcmp_stb STATIC src/stb_impl.cpp)

# SYSTEM: vendored headers are held to upstream's warning bar, not ours
# (architecture.md §8). This target deliberately does NOT link engine::warnings.
target_include_directories(engine_imgcmp_stb SYSTEM PUBLIC ${ENG_THIRD_PARTY_DIR}/stb)

# PUBLIC because STBI_NO_STDIO changes the declarations too: every translation
# unit that sees the headers must see the same configuration.
target_compile_definitions(engine_imgcmp_stb PUBLIC
    STBI_ONLY_PNG        # screenshots are PNG; the other decoders are only attack surface
    STBI_NO_STDIO        # all file I/O goes through std::filesystem in image.cpp
    STBI_WRITE_NO_STDIO
)
if(MSVC)
    # stb calls the CRT's unchecked string functions by design.
    target_compile_definitions(engine_imgcmp_stb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# The tool
# ---------------------------------------------------------------------------
# The logic lives in a library so the tests exercise the same code the binary
# runs; main.cpp is only argv plumbing and exit codes.
add_library(engine_imgcmp_lib STATIC
    src/command_line.cpp
    src/compare.cpp
    src/holes.cpp
    src/image.cpp
)
target_include_directories(engine_imgcmp_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# PRIVATE: stb appears in no header of ours, so consumers never see it.
target_link_libraries(engine_imgcmp_lib PRIVATE engine::warnings engine_imgcmp_stb)

add_executable(imgcmp src/main.cpp)
target_link_libraries(imgcmp PRIVATE engine::warnings engine_imgcmp_lib)

# `imgstrip` lays several screenshots side by side into one image, which is how a
# claim about change over time -- a sun crossing the sky, a part fading -- gets
# shown as one thing a reviewer can look at. Same library, same independence from
# `engine/`, and no test target of its own: it is fifty lines of copying pixels
# and the library underneath it is already covered.
add_executable(imgstrip src/strip_main.cpp)
target_link_libraries(imgstrip PRIVATE engine::warnings engine_imgcmp_lib)

# `imgprobe` asserts the colour at named points of a screenshot -- the shape of
# a claim about one feature ("the bar's left end is red"), which a golden image
# of the whole frame over-states. Same library, same independence.
add_executable(imgprobe src/probe_main.cpp)
target_link_libraries(imgprobe PRIVATE engine::warnings engine_imgcmp_lib)

# `imgholes` counts where a picture of the ground shows the sky through it, or
# instead of it -- the terrain gallery's check (terrain audit T0). Same library,
# same independence.
add_executable(imgholes src/holes_main.cpp)
target_link_libraries(imgholes PRIVATE engine::warnings engine_imgcmp_lib)

if(ENG_BUILD_TESTS)
    add_executable(engine_imgcmp_tests
        tests/command_line_tests.cpp
        tests/compare_tests.cpp
        tests/holes_tests.cpp
        tests/image_tests.cpp
    )
    target_link_libraries(engine_imgcmp_tests PRIVATE
        engine::warnings
        doctest::doctest_with_main
        engine_imgcmp_lib
    )
    add_test(NAME imgcmp COMMAND engine_imgcmp_tests)
endif()
