# The pipelines a run made are kept even when the run is killed (ADR 0257).
#
# A phone, a headset or a player with a task manager ends a game without a
# word, and a cache written only when the device is destroyed was never
# written there. The run below is killed, not quit, a while after a surface
# shader new to the driver was first worn; what it made must be on disk, whole,
# and the next run seeded by it.
#
# On Vulkan alone, the one device that keeps a cache; skipped where there is
# none.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_pipeline_cache_kill.cmake
set(ENG_NO_DEVICE_EXIT_CODE 4)
# The blocks come on at four seconds of play, their pipelines are made within
# a couple, and kept five seconds after the last: killed well after that.
set(ENG_KILL_AFTER_SECONDS 45)
set(ENV{ENG_LOG_KEYS} 1)
set(ENV{SDL_GPU_DRIVER} vulkan)
foreach(required HOST SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_pipeline_cache_kill.cmake: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/game"
    PATTERN ".engine" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)
string(RANDOM LENGTH 9 ALPHABET "0123456789" nonce)
file(READ "${OUTPUT}/game/content/shaders/look.surface.hlsl" shader)
string(REPLACE "0.0 * 1.0e-9" "0.${nonce} * 1.0e-9" shader "${shader}")
file(WRITE "${OUTPUT}/game/content/shaders/look.surface.hlsl" "${shader}")
set(cache "${OUTPUT}/pipelines.cache")

# Killed: no `--exit`, frames enough for minutes, and the timeout ends it.
execute_process(
    COMMAND "${HOST}" "${OUTPUT}/game" --headless --pipelines=off-frame "--pipeline-cache=${cache}"
            --frames=100000 --pace=60 --width=320 --height=180
    TIMEOUT ${ENG_KILL_AFTER_SECONDS}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE OR NOT output MATCHES "through vulkan")
    message("ENG_TEST_SKIP: no Vulkan device on this machine")
    return()
endif()
if(output MATCHES "No shader compiler was found")
    message("ENG_TEST_SKIP: no shader compiler on this host")
    return()
endif()
# **A software rasterizer is not what this measures.** llvmpipe compiles a
# shader the first time it draws with it, whatever made the pipeline before,
# and keeps nothing in a pipeline cache: on it the frame waits in its present
# and the cache file is a header alone, and neither says anything of a GPU.
if(output MATCHES "Drawing on (llvmpipe|lavapipe|SwiftShader)")
    message("ENG_TEST_SKIP: a software rasterizer compiles a shader at its first draw and caches nothing")
    return()
endif()
if(NOT output MATCHES "render\\.info\\.surface_pipelines_off_frame")
    message("${output}")
    message(FATAL_ERROR "pipeline cache kill: the shader's pipelines were never made before the kill")
endif()

# Whole: the mark the file begins with, and more than a header.
if(NOT EXISTS "${cache}")
    message("${output}")
    message(FATAL_ERROR "pipeline cache kill: a run killed after making pipelines left no cache file")
endif()
file(SIZE "${cache}" size)
# Read as hex: the bytes after the mark are binary.
file(READ "${cache}" mark LIMIT 8 HEX)
if(NOT mark STREQUAL "53444c4750555043" OR size LESS 1024)
    message(FATAL_ERROR "pipeline cache kill: the cache file is not a whole one (${size} bytes, begins ${mark})")
endif()

# And the next run is seeded by it.
execute_process(
    COMMAND "${HOST}" "${OUTPUT}/game" --headless --pipelines=on-frame "--pipeline-cache=${cache}" --frames=30
            --exit --width=320 --height=180
    RESULT_VARIABLE result
    OUTPUT_VARIABLE again
    ERROR_VARIABLE again)
if(NOT result EQUAL 0)
    message("${again}")
    message(FATAL_ERROR "pipeline cache kill: the second run failed (${result})")
endif()
string(REGEX MATCH "pipeline_cache_seeded\\] [^0-9]*([0-9]+) bytes" seeded "${again}")
if(seeded STREQUAL "" OR CMAKE_MATCH_1 EQUAL 0)
    message("${again}")
    message(FATAL_ERROR "pipeline cache kill: the next run was not seeded by the file the killed one left")
endif()
message("pipeline cache kill: killed with ${size} bytes kept; the next run was seeded with ${CMAKE_MATCH_1}")
