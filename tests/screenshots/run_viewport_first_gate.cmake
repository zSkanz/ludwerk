# A ViewportFrame is never a white square, the frame it is first shown (D575).
#
# Three ViewportFrames made at tick 20 over a blue panel, a red crate in each.
# The run is photographed at every frame from the one before they are made to
# a few after, and in every picture each slot's middle is the panel's blue --
# its picture not there yet, the frame's own background showing through -- or
# the crate's red. White in any of them is the defect: the draw list named the
# view's picture by a place the frame's texture table did not have until the
# frame after, and a place past the table's end is drawn white.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_viewport_first_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_viewport_first_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

set(shown 0)
foreach(frames RANGE 20 26)
    set(picture "${OUTPUT}/frame${frames}.png")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit "--screenshot=${picture}"
                --width=600 --height=300
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${picture}")
        message("${output}")
        message(FATAL_ERROR "viewport first shown: the host failed at ${frames} frames (${result})")
    endif()

    # The three slots' middles, and none of them white -- nor anything near it:
    # the tolerance is wide enough that the blue and the red both pass.
    execute_process(
        COMMAND "${PROBE}" "${picture}" --tolerance=90 "0.175,0.5!=255,255,255" "0.475,0.5!=255,255,255"
                "0.775,0.5!=255,255,255"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "viewport first shown: a slot was white at ${frames} frames; the pictures are in ${OUTPUT}")
    endif()

    # And by the last of them the crates are there.
    execute_process(
        COMMAND "${PROBE}" "${picture}" --tolerance=70 "0.175,0.5=190,45,55" "0.475,0.5=190,45,55"
                "0.775,0.5=190,45,55"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL 0)
        math(EXPR shown "${shown} + 1")
    endif()
endforeach()

if(shown LESS 1)
    message(FATAL_ERROR "viewport first shown: no picture has the crates in it; the scene is wrong")
endif()
message("viewport first shown: never white, and the crates are in ${shown} of the pictures")
