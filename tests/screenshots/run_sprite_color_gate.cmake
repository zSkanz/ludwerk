# Are a sprite's colours on the screen the colours it was given? (ADR 0153)
#
# Probes, as `run_ui_appearance_gate.cmake` is and for its reason:
# `tests/screenshots/spritecolor` puts one flat square per claim at a known
# place, and each probe below is one claim. **The tolerance is one step**: the
# claim is that the bytes arrive, not that something like them does.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_sprite_color_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_sprite_color_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=5 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "sprite colour gate: the host exited ${host_result} and wrote no screenshot")
endif()

# Twenty metres across 1000 pixels and ten down 500: x = (metres + 10) / 20,
# y = (5 - metres) / 10. The scene's comment names every square.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=1
        # The top row: each colour as its own bytes.
        "0.1,0.3=200,80,40"
        "0.25,0.3=255,255,255"
        "0.4,0.3=128,128,128"
        "0.55,0.3=10,200,250"
        "0.7,0.3=6,4,2"
        # A picture of one flat colour, through the texture.
        "0.85,0.3=90,170,60"
        # Lit: the same orange is NOT those bytes once the scene has exposed it.
        "0.1,0.7!=200,80,40"
        # Half a black over a white, mixed as light: linear 0.5, which is 188.
        "0.3,0.7=188,188,188"
        # A tile is a sprite: cell (1, -2) of two metres, middle at (3, -3).
        "0.65,0.8=90,170,60"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "sprite colour gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
