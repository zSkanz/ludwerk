# A terrain layer's repeat, broken up (ADR 0113's amendment): does the
# built-in grass still read as a grid?
#
# One run of the scene in `--debug-view=albedo` -- the colour alone, so the
# sun's shading is not measured -- photographing three fields in order
# (`--screenshot-every`), each straight down from 12 m and from 60 m and once
# across it:
#
#   0-2  `repeating-grass`: the variation and the far sample turned off, the
#        layer as every terrain drew it before;
#   3-5  the built-in grass as it is, broken up by default;
#   6-8  `hex-grass`: the same on hexagonal cells.
#
# `imgtile` measures each picture from above: the autocorrelation of its detail
# at the grass's 3 m repeat (96.5 px from 12 m, 19.3 px from 60 m, at a 50
# degree field of view and 360 rows), from 60 m also at the far sample's 18 m
# in any direction, and how much period-sized blocks vary in colour. **The same
# limits are held to all three fields, and the first must fail them**: that is
# the proof the measure sees a repeat at all.
#
# Measured on the machine that made the change: repeating 0.99 near and 0.98
# far, blocks 0.008; broken up 0.50 far, 0.23 at 18 m, blocks 0.054; hex 0.05
# near, 0.01 far. **Near, the default is not held to the far limits** -- 0.87
# from 12 m: the far sample and the variation do not hide a motif as bold as
# the grass's dry patches in a picture four repeats across. That is what hex
# tiling is for, and the default is held only to not going back.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DTILE=<imgtile> -DSCRIPT=<project> -DOUTPUT=<dir>
#         -P run_terrain_tiling.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# The scene's own `FramesPerCase`, and its case count.
set(FRAMES_PER_CASE 20)
set(CASES 9)
set(NEAR_PERIOD 96.5)
set(FAR_PERIOD 19.3)
set(WIDE_PERIOD 115.8)

foreach(required HOST TILE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_tiling.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
math(EXPR frames "${CASES} * ${FRAMES_PER_CASE}")

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit "--screenshot=${OUTPUT}/albedo.png"
            "--screenshot-every=${FRAMES_PER_CASE}" --debug-view=albedo --width=640 --height=360
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "terrain tiling: the host failed (${result})")
endif()

set(failed 0)
# measure(<case> <expect pass|fail> <imgtile arguments>...)
function(measure index expect)
    set(shot "${OUTPUT}/albedo-00${index}.png")
    if(NOT EXISTS "${shot}")
        message(FATAL_ERROR "terrain tiling: case ${index} was not photographed (${shot})")
    endif()
    execute_process(
        COMMAND "${TILE}" "${shot}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${output}")
    if(result GREATER 1)
        message(FATAL_ERROR "terrain tiling: imgtile could not measure ${shot}")
    endif()
    if((expect STREQUAL "pass" AND NOT result EQUAL 0) OR (expect STREQUAL "fail" AND NOT result EQUAL 1))
        message("terrain tiling: case ${index} was expected to ${expect}")
        math(EXPR count "${failed} + 1")
        set(failed ${count} PARENT_SCOPE)
    endif()
endfunction()

# From 60 m: broken up, at the fine repeat and the far sample's, and varied.
set(far_limits "--period=${FAR_PERIOD}" --max-repeat=0.65 --min-blocks=0.025)
set(wide_limits "--period=${WIDE_PERIOD}" --max-repeat=0.5)
measure(1 fail ${far_limits})
measure(4 pass ${far_limits})
measure(4 pass ${wide_limits})
measure(7 pass ${far_limits})
measure(7 pass ${wide_limits})
# From 12 m: hex breaks it; the default must not go back.
measure(0 fail "--period=${NEAR_PERIOD}" --max-repeat=0.3)
measure(6 pass "--period=${NEAR_PERIOD}" --max-repeat=0.3)
measure(3 pass "--period=${NEAR_PERIOD}" --max-repeat=0.93)

if(failed GREATER 0)
    # One line a test's expectation can match: FATAL_ERROR's text is wrapped.
    message("ENG_TERRAIN_TILING_FAULT: ${failed}")
    message(FATAL_ERROR "terrain tiling: ${failed} picture(s) measured against what was expected")
endif()
