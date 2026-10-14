# Does a sub-world draw its own world into its own view (ADR 0107 §3)?
#
# `tests/screenshots/subworld`: two `SubWorld`s running one scene -- a camera
# facing a green wall -- each shown on half of the screen. The right one was
# sent "blue" and turned its wall blue; the left one was not. The host's own
# red wall, where both cameras look, is in neither picture. Probes, not a
# golden, for the reason `run_ui_appearance_gate.cmake` gives.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_subworld_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_subworld_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=10 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "subworld gate: the host exited ${host_result} and wrote no screenshot")
endif()

# x,y as fractions of 1000 by 500.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40
        # Left: the scene as it starts, green.
        "0.25,0.5=3,205,27"
        "0.1,0.1=3,205,27"
        # Right: the same scene, told "blue" by the host.
        "0.75,0.5=107,108,251"
        "0.9,0.9=107,108,251"
        # And the host's red wall in neither.
        "0.25,0.5!=220,0,0"
        "0.75,0.5!=220,0,0"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "subworld gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
