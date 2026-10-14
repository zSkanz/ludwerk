# A terrain layer whose material gives off light glows (ADR 0113, amended).
#
# Flat ground seen from above, rock on the left of the picture and lava on the
# right, with nothing lighting either. The rock is black; the lava is its
# material's `Emissive`, red over green over blue. Photographed with the
# full ground, the lean one and the fast one -- another shader: a ground that
# glowed in one and not the other would be a volcano that goes dark on a phone.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_terrain_glow.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_glow.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

foreach(ground full lean fast)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=30 --exit "--screenshot=${OUTPUT}/${ground}.png"
                --width=640 --height=360 "--terrain-surface=${ground}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${ground}.png")
        message("${output}")
        message(FATAL_ERROR "terrain glow: the host failed on the ${ground} ground (${result})")
    endif()

    # The rock is black, the lava brighter than it by far, and redder than it
    # is green or blue: at a quarter and three quarters of the way across.
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${ground}.png" --tolerance=60
                "0.25,0.5=0,0,0" "0.75,0.5>0.25,0.5" "0.75,0.5!=0,0,0"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${ground}: ${output}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "terrain glow: the lava does not glow on the ${ground} ground")
    endif()
endforeach()
