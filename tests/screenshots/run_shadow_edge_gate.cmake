# The edge of the sun's shadow is a ramp, not a comb.
#
# The sun's shadow is filtered by sixteen taps on a disc, and the disc was
# turned by a different angle at every pixel -- to break up the steps a
# kernel with gaps in it draws across a wide penumbra. But each tap is a
# comparison two texels across, and where the taps stand closer than that the
# disc has no gaps to hide: turned, every pixel's answer was a little off its
# neighbour's, a fringe of hairs a pixel wide along every shadow's edge, still
# for ever. Only a temporal pass hid it, so nobody who ran one saw it.
#
# Asked of `examples/39-interior`, where it was seen: the patch the sun lays
# on the floor through the window, close to the camera -- so a texel of the
# shadow is smaller than a pixel and the kernel is at its widest -- drawn as
# the shadow's own term (`--debug-view=shadow`), which nothing else is in.
# Along the patch's near edge a pixel that stands out from both its
# neighbours the same way is a tooth (`imgprobe`'s `^`). The corrected filter
# leaves 8 in the 6,900 pixels asked, where the edge steps a row; the turned
# one left 53.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<examples/39-interior>
#         -DOUTPUT=<png> -P run_shadow_edge_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_shadow_edge_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=40 --exit --locale=en --width=1920 --height=1080
            --quality=high --debug-view=shadow "--screenshot=${OUTPUT}"
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
message("${host_output}")
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "shadow edge gate: the host exited ${host_result} and wrote no picture")
endif()

# x,y as fractions of 1920 by 1080: the near edge of the sun's patch.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=18
        "0.677,0.616:0.729,0.68^20"
        # And the region is that edge: lit above it, shadowed below.
        "0.70,0.62>0.70,0.675"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR
        "shadow edge gate: the edge of the sun's shadow is a comb, or is not where it was asked (see above).\n"
        "A filter that differs from pixel to pixel draws one. The picture is ${OUTPUT}.")
endif()
message("shadow edge gate: the edge of the sun's patch is a ramp")
