# The face example runs, and its faces move (ADR 0196).
#
# `examples/36-face` for a second and a half: three busts of one model with
# five shape keys. One talks by its clip's weight channels, one is moved by
# the example's script through `MeshPart:SetMorphWeight`, and one is at rest.
# So the frame report must count the two that move as `morph` draws -- a draw
# a material, four each -- and must not count the third; and nothing may warn:
# a key the script names that the model lacks is an error, and a model that
# did not load is a picture of three stands.
#
# What a face looks like is the example's to show a person; what is held here
# is that the two ways of setting a weight both reach the renderer in the
# example as it ships.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<examples/36-face> -P run_face_example.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST SCRIPT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_face_example.cmake: -D${required}=... is required")
    endif()
endforeach()

execute_process(
    # A small picture, as the cape's: on a runner with no graphics card every
    # pixel is a processor's, and what is tested is the draws.
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=100 --exit --pace=60 --frame-report=1
            --width=320 --height=180 --quality=low
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "face example: the host failed (${result})")
endif()
string(FIND "${output}" "[error]" errorAt)
string(FIND "${output}" "[warn]" warnAt)
if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "face example: the run warned or failed")
endif()

if(NOT output MATCHES "by what each was for: ([a-z_0-9, ]*)")
    message("${output}")
    message(FATAL_ERROR "face example: no frame report with its draws by kind")
endif()
set(kinds "${CMAKE_MATCH_1}")
message("face example: ${kinds}")
if(NOT kinds MATCHES "morph ([0-9]+)")
    message(FATAL_ERROR "face example: no bust was drawn through the morph pipelines")
endif()
if(NOT CMAKE_MATCH_1 EQUAL 8)
    message(FATAL_ERROR
        "face example: two busts of four materials move, eight morph draws, and ${CMAKE_MATCH_1} were counted -- "
        "the third bust is at rest and is none")
endif()
