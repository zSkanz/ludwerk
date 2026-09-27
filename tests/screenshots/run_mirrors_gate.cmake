# Does a mirror show what a mirror would, and a portal what is beyond it
# (ADR 0107, `@engine/views`)?
#
# `tests/screenshots/mirrors`: a mirror on the left showing a red and a blue
# block that stand BEHIND the camera -- red on the left, as a mirror turns
# them -- and not the green block standing behind the glass, which only the
# camera's clip plane keeps out; a portal on the right showing the yellow block
# in front of its far pane and not the purple one behind it. Probes, not a
# golden, for the reason `run_ui_appearance_gate.cmake` gives.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_mirrors_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_mirrors_gate.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "mirrors gate: the host exited ${host_result} and wrote no screenshot")
endif()

# x,y as fractions of 1000 by 500.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40
        # The mirror: red on the left, blue on the right -- behind the camera,
        # and turned the way a mirror turns them.
        "0.363,0.5=175,0,15"
        "0.43,0.5=1,8,218"
        # And no green anywhere the green block would fill without the clip.
        "0.25,0.35!=0,200,0"
        "0.36,0.66!=0,200,0"
        "0.363,0.5!=0,200,0"
        # The portal: the yellow block beyond the far pane, and not the purple
        # one behind it.
        "0.645,0.5=172,190,0"
        "0.78,0.5!=153,0,255"
        "0.78,0.5!=172,190,0"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "mirrors gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
