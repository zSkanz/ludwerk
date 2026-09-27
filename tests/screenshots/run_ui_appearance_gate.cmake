# Does a `UIGradient` colour, and a `UIStroke` outline, where ADR 0110 says?
#
# Probes, not a golden: `tests/screenshots/uiappearance` lays out one flat case
# per claim at known pixels, and each probe below is one claim -- a colour at a
# point, or the absence of one. A golden of the whole frame would also be
# asserting the font's hinting and the backdrop's exact shade, and would be
# replaced the first time either moved; a probe holds until the claim breaks.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_ui_appearance_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_ui_appearance_gate.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "ui appearance gate: the host exited ${host_result} and wrote no screenshot")
endif()

# Each probe is x,y as fractions of 1000 by 500, then the colour; `!=` is a
# colour that must NOT be there. The scene's comment names every box.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}"
        # 1. Linear: red at the left end, green at the right.
        "0.05,0.12=249,6,0"
        "0.43,0.12=6,249,0"
        # 2. Radial: red at the centre, nearly blue at nine tenths of the radius.
        "0.14,0.44=255,0,0"
        "0.23,0.44=26,0,229"
        # 3. Conical: red where it starts (to the right), blue half a turn on.
        "0.61,0.2=255,0,0"
        "0.51,0.2=0,0,255"
        # 4. Repeat at half scale: nearly white just before the middle, nearly
        #    black just after it -- where a clamped gradient would stay white.
        "0.755,0.1=242,242,242"
        "0.765,0.1=13,13,13"
        # 5. Transparency: opaque white at the left, nearly the backdrop at the
        #    right.
        "0.67,0.22=243,243,243"
        "0.85,0.22=31,31,40"
        # 6. Outer: yellow outside the box, grey just inside. Inner: yellow just
        #    inside, the backdrop just outside.
        "0.295,0.46=255,220,0"
        "0.305,0.46=60,60,60"
        "0.465,0.46=255,220,0"
        "0.455,0.46=20,20,30"
        # 7. Ten pixels out from a corner: filled by a mitred stroke, empty
        #    beside a round one.
        "0.29,0.66=255,220,0"
        "0.45,0.66=20,20,30"
        # 8. A stroke's own gradient: red on its left side, blue on its right.
        "0.655,0.74=243,0,12"
        "0.865,0.74=12,0,243"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "ui appearance gate: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()
