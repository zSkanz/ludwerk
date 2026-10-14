# The effects gate (ADR 0129): `Highlight`, `Beam` and `Trail`, drawn by a real
# device and asked what colour each place is.
#
# `tests/screenshots/effects` is the scene and says where everything is. What
# is asked here:
#
#   - a highlight set `AlwaysOnTop` shows through the wall in front of its
#     shape, in exactly the colour it was given -- it is drawn over the
#     finished picture, so no light, exposure or tone curve has touched it;
#   - one set `Occluded` behind a wall marks nothing, and in plain sight marks
#     its shape and draws its line round it;
#   - a beam is a band between its two attachments and nothing outside it;
#   - a trail covers where its attachments have been and not where they have
#     not.
#
# No checked-in picture: a probe says what a place must be, which survives a
# change to how a scene is lit and fails on what this gate is about.
#
# Usage, from CTest:
#   cmake -DHOST=... -DPROBE=... -DSCRIPT=... -DOUTPUT=... -P run_effects_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_effects_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=40 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "effects gate: the host exited ${host_result} and wrote no screenshot")
endif()

# The highlights are the colours they were given, to the byte.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=2
        # Behind a wall, `AlwaysOnTop`: the fill, through the wall.
        "0.125,0.25=255,255,0"
        # In plain sight, `Occluded`: the fill, and the line round its edge
        # -- to its right and above it.
        "0.625,0.25=0,255,255"
        "0.6765,0.25=255,0,0"
        "0.625,0.1485=255,0,0"
        # Behind a wall, `Occluded`: neither.
        "0.375,0.25!=255,255,0"
        "0.375,0.25!=255,0,0"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "effects gate: a highlight probe disagreed (see above); the frame is ${OUTPUT}")
endif()

# The ribbons are lit things under a tone curve: asked more loosely.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40
        # The hidden cube's wall is the wall, as the one beside it is.
        "0.375,0.25=100,114,227"
        "0.05,0.1=100,114,227"
        # The beam: green in its band, the back wall above and below it.
        "0.875,0.15=84,249,84"
        "0.875,0.05=162,180,210"
        "0.875,0.3=162,180,210"
        # The trail: magenta where the runner has been, wall ahead of it.
        "0.25,0.75=249,84,249"
        "0.1,0.75=249,84,249"
        "0.65,0.75=162,180,210"
        # And not the other way round.
        "0.875,0.15!=162,180,210"
        "0.25,0.75!=162,180,210"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "effects gate: a ribbon probe disagreed (see above); the frame is ${OUTPUT}")
endif()
