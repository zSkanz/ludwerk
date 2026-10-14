# Do ADR 0128's layouts, constraints, canvas group and selection outline draw
# where the ADR says?
#
# Probes, as `run_ui_appearance_gate.cmake` is and for its reason:
# `tests/screenshots/uilayouts` lays out one flat case per claim at known
# pixels, and each probe below is one claim.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_ui_layouts_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_ui_layouts_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=3 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "ui layouts gate: the host exited ${host_result} and wrote no screenshot")
endif()

# Each probe is x,y as fractions of 1000 by 500, then the colour; `!=` is a
# colour that must NOT be there. The scene's comment names every box.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}"
        # 1. A grid: the second cell, the gap after it, the fourth cell on the
        #    next row, and no fifth beside it.
        "0.11,0.12=255,0,0"
        "0.135,0.12=60,60,60"
        "0.06,0.22=255,0,0"
        "0.11,0.22=60,60,60"
        # 2. Pages: the second is shown, and its neighbours are clipped away.
        "0.29,0.14=0,255,0"
        "0.36,0.14=20,20,30"
        "0.225,0.14=20,20,30"
        # 3. Flex: a child at each end and one in the middle, the holder between.
        "0.41,0.11=255,255,255"
        "0.5,0.11=255,255,255"
        "0.59,0.11=255,255,255"
        "0.455,0.11=60,60,60"
        # 4. A scale: yellow out to twice the box, and not past it.
        "0.71,0.22=255,220,0"
        "0.73,0.22=20,20,30"
        # 5. A shape: square, so the right half of its holder shows.
        "0.79,0.14=0,255,0"
        "0.85,0.14=60,60,60"
        # 6. A size limit: 50 by 30, and nothing of the 200 it asked for.
        "0.06,0.43=255,255,255"
        "0.1,0.43=20,20,30"
        "0.06,0.48=20,20,30"
        # 7. A canvas group: where its children overlap it is the blue one
        #    alone at half over white -- the reference's colour -- and the
        #    control, faded child by child, is not.
        "0.26,0.48=255,127,127"
        "0.32,0.48=127,127,255"
        "0.69,0.48=127,127,255"
        "0.52,0.48!=127,127,255"
        # 8. The selection: its outline just outside the button, the button
        #    inside.
        "0.0365,0.72=64,158,255"
        "0.06,0.72=60,60,60"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "ui layouts gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
