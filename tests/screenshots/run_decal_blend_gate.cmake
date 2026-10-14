# A decal's three blend modes (ADR 0160), asked of a screenshot: a multiplied
# decal darkens the floor it lands on, and one laid over it glowing, or added
# to it, leaves it brighter -- which a multiply never could. Comparisons of
# this frame's own pixels, so the light and the tone curve are no part of the
# claim, and it is a gate wherever there is a device.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_decal_blend_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_decal_blend_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=20 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
            --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "decal blend gate: the host exited ${host_result} and wrote no screenshot")
endif()

# Twenty metres across 1000 pixels: a square's middle at x = (metres + 10) / 20,
# on the picture's middle row; the bare floor between two of them at x = 0.35
# and beyond them at 0.95.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=12
        # Multiply: darker than the floor beside it.
        "0.2,0.5<0.35,0.5"
        # Alpha, glowing: brighter than the floor.
        "0.5,0.5>0.35,0.5"
        # Additive: brighter than the floor.
        "0.8,0.5>0.95,0.5"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "decal blend gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
