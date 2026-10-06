# A picture drawn small in the interface is drawn from its smaller levels (D578).
#
# A checkerboard of single texels, 256 across, shown 17, 22, 28 and 44 pixels
# tall on a dark screen. Every pixel inside each is the board's mean -- an even
# grey, half as bright as white, which is 188 -- because a picture drawn small
# is sampled from a level near the size it is drawn at. It went up at its top
# level alone, and a sampler reads four texels of the fifteen by fifteen a
# pixel then covers: the squares were black and white in a pattern of their
# own.
#
# Asked as "every pixel here is this grey" and not as a golden: the claim is
# that nothing of the board's texels shows, on any rasteriser.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_ui_minify_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_ui_minify_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=5 --exit "--screenshot=${OUTPUT}" --width=640 --height=360
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message("${output}")
    message(FATAL_ERROR "ui minify: the host failed (${result})")
endif()

# Each square two pixels in from its edges, as fractions of 640 by 360: at
# x 100, 200, 300 and 400, y 100, of 17, 22, 28 and 44 pixels.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=20
            "0.1594,0.2833:0.1797,0.3194~188,188,188"
            "0.3156,0.2833:0.3438,0.3333~188,188,188"
            "0.4719,0.2833:0.5094,0.3500~188,188,188"
            "0.6281,0.2833:0.6906,0.3944~188,188,188"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "ui minify: a picture drawn small shows its texels and not its mean")
endif()
