# A surface shader first worn in play costs that frame nothing (ADR 0257).
#
# Five blocks wearing a material whose surface shader nothing has drawn come
# on after four seconds of play. The shader carries a number this run makes
# up, so its bytecode is new and the driver makes its pipelines cold -- tenths
# of a second for a large shader, all of it on the frame that first drew one
# when they were made there.
#
# Read from what the host says:
#   - the pipelines were made off the frame, and said so;
#   - no frame from the blocks' arrival to the end took
#     `ENG_FIRST_USE_FRAME_MS` or more.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_surface_first_use.cmake
set(ENG_NO_DEVICE_EXIT_CODE 4)
# A hitch a player sees. The frames of the stretch are 17 ms at the pace
# asked for; one of 300 is the driver's compile, not a busy machine.
set(ENG_FIRST_USE_FRAME_MS 300)
# The host is read by the keys of what it says, not by the words (`--log-keys`).
set(ENV{ENG_LOG_KEYS} 1)
foreach(required HOST SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_surface_first_use.cmake: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/game"
    PATTERN ".engine" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)
# This run's own number: new bytecode, a cold driver.
string(RANDOM LENGTH 9 ALPHABET "0123456789" nonce)
file(READ "${OUTPUT}/game/content/shaders/look.surface.hlsl" shader)
string(REPLACE "0.0 * 1.0e-9" "0.${nonce} * 1.0e-9" shader "${shader}")
file(WRITE "${OUTPUT}/game/content/shaders/look.surface.hlsl" "${shader}")

execute_process(
    COMMAND "${HOST}" "${OUTPUT}/game" --headless --pipelines=off-frame --frames=720 --exit --pace=60
            --frame-report=1 --width=320 --height=180
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
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
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "surface first use: the host failed (${result})")
endif()
string(FIND "${output}" "[error]" errorAt)
if(NOT errorAt EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "surface first use: the run failed")
endif()

string(FIND "${output}" "MATERIAL ON" on)
if(on EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "surface first use: the blocks never came on")
endif()
string(SUBSTRING "${output}" ${on} -1 after)
if(NOT after MATCHES "render\\.info\\.surface_pipelines_off_frame")
    message("${output}")
    message(FATAL_ERROR "surface first use: the shader's pipelines were not made off the frame")
endif()
string(REGEX MATCHALL "worst [0-9.]+ ms" worsts "${after}")
list(LENGTH worsts count)
if(count EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "surface first use: no frame report after the blocks came on")
endif()
set(worstMs 0)
foreach(line ${worsts})
    string(REGEX MATCH "([0-9]+)" found "${line}")
    if(CMAKE_MATCH_1 GREATER worstMs)
        set(worstMs ${CMAKE_MATCH_1})
    endif()
endforeach()
string(REGEX MATCH "surface_pipelines_off_frame[^\n]*" made "${after}")
message("surface first use: ${made}; the worst frame after the blocks came on took ${worstMs} ms")
if(NOT worstMs LESS ENG_FIRST_USE_FRAME_MS)
    message("${output}")
    message(FATAL_ERROR
        "surface first use: a frame took ${worstMs} ms after the blocks came on, at or past "
        "${ENG_FIRST_USE_FRAME_MS}: the shader's pipelines were waited for on the frame")
endif()
