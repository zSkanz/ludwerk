# Does an orthographic camera show everything in its column, whatever its
# shape, however low the camera stands (the owner's report, 2026-09-27)?
#
# `tests/screenshots/ortho`: a top-down orthographic camera a metre up, over a
# ball two metres up, blocks, a cylinder and a wedge. Every one must be in the
# picture: the ball once vanished and left only its shadow. Probes, not a
# golden, for the reason `run_ui_appearance_gate.cmake` gives.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_ortho_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_ortho_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=4 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "ortho gate: the host exited ${host_result} and wrote no screenshot")
endif()

# x,y as fractions of 1000 by 500.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40
        # The ball, wholly above the camera.
        "0.437,0.792=243,194,231"
        # A block, whose top is above it.
        "0.36,0.84=136,196,250"
        # The cylinder and the wedge, taller than where the camera stands.
        "0.5,0.332=244,236,140"
        "0.562,0.332=236,230,130"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "ortho gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
