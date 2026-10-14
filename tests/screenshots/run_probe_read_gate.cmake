# A nearly mirror floor does not show the grid of the probe it reflects.
#
# A `ReflectionProbe` is one map a hundred and twenty-eight texels a side for
# the whole sphere: a texel is a degree and a half. A polished floor reads its
# sharpest level magnified twenty times, and read plainly -- a ramp between
# four texels along each axis, with a crease where two ramps meet -- an edge
# that ran across the map's grid came out as the grid: a fringe of feathers
# hanging under every bright thing the floor reflected, each a few pixels wide
# and a dozen long. The probe's first level is read through a cubic now, which
# has no crease.
#
# Asked of `examples/39-interior`, where it was seen: the floor just under
# the near edge of the sun's patch, where the floor reflects the window's
# frame. A pixel that stands out from both the pixels four to its left and
# right the same way, or four above and below, is a tooth of that comb
# (`imgprobe`'s `^N/L`). Read plainly there are 66 in the 7,680 pixels asked;
# through the cubic, 3; with no probe at all, none.
#
# Which pass it was is in the row: nothing changed it but the probes -- gone
# with none, coarser with probes of sixty-four.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<examples/39-interior>
#         -DOUTPUT=<png> -P run_probe_read_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_probe_read_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
# The screen's reflections off: this is the probe's own picture in the floor.
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=60 --exit --locale=en --width=1920 --height=1080
            --quality=high --screen-reflections=low --screen-bounce=low "--screenshot=${OUTPUT}"
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "probe read gate: the host exited ${host_result} and wrote no picture")
endif()

# x,y as fractions of 1920 by 1080.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=9
        "0.70,0.703:0.86,0.735^15/4"
        # And the place is the one meant: the sun's patch is above it, brighter.
        "0.78,0.66>0.78,0.72"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR
        "probe read gate: the floor under the sun's patch shows a comb, or is not where it was asked (see\n"
        "above). A probe's first level read plainly draws its own grid there. The picture is ${OUTPUT}.")
endif()
message("probe read gate: the floor reflects its probe with no grid")
