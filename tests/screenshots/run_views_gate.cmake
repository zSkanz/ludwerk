# Do two cameras draw into two textures in one frame, and does a picture show
# each (ADR 0107)?
#
# `tests/screenshots/views` puts a green wall in front of the world's camera and
# a red and a blue wall in front of two cameras whose `CameraTexture`s draw into
# `view://red` and `view://blue`; two `ImageLabel`s show those names. Probes,
# not a golden, for the reason `run_ui_appearance_gate.cmake` gives. The feeds
# are lighter than the world's view, and that is the claim too: each view keeps
# its own exposure, and a camera that sees only a dark red wall opens up. And a
# `ViewportFrame` with no background shows its cube and, round it, the world.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_views_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_views_gate.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "views gate: the host exited ${host_result} and wrote no screenshot")
endif()

# x,y as fractions of 1000 by 500; the scene's comment names every box.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40
        # The world's own view: the green wall, between and below the pictures.
        "0.25,0.8=2,203,19"
        "0.5,0.38=2,203,19"
        # The left picture is the red camera's feed, the right the blue one's.
        "0.22,0.38=248,74,78"
        "0.78,0.38=117,118,252"
        # And neither is the other, nor the world behind them.
        "0.22,0.38!=117,118,252"
        "0.78,0.38!=248,74,78"
        "0.22,0.38!=2,203,19"
        # A ViewportFrame with no background: its yellow cube in the middle, and
        # the world's green wall through the part of the frame it left clear.
        "0.5,0.78=196,202,2"
        "0.41,0.62=2,203,19"
        "0.41,0.62!=196,202,2"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "views gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
