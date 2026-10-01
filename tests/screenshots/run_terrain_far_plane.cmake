# Ground wider than the camera sees (D403): does it reach the far plane across
# the whole picture, and end there in a line?
#
# The project's script lays a flat slab 12 km across and looks over it from
# 400 m up, pitched 10 degrees down, through 50 degrees, seeing 5 000 m.
# Photographed in `--debug-view=holes`: ground one grey, sky magenta. Flat
# ground under a pitched camera ends at the far plane on one row of the
# picture, 0.398 of the way down; so a row under it (0.415) is ground from one
# side to the other, and a row over it (0.38) is sky.
#
# Before D403 the ground was drawn to 4 096 m from the camera, by whole nodes:
# at the sides of the picture, where a point inside the far plane is further
# from the camera than one at the middle, the row under the line was sky.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project> -DOUTPUT=<dir>
#         -P run_terrain_far_plane.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_far_plane.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

# Frames enough for the far nodes, built off the frame's thread, to be drawn.
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=360 --pace=60 --exit "--screenshot=${OUTPUT}/far-plane.png"
            --debug-view=holes --width=640 --height=360
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/far-plane.png")
    message("${output}")
    message(FATAL_ERROR "terrain far plane: the host failed (${result})")
endif()

# The sky is the view's magenta as the post chain leaves it; ground is not.
set(probes "")
foreach(x 0.03 0.15 0.30 0.50 0.70 0.85 0.97)
    list(APPEND probes "${x},0.415!=179,0,179" "${x},0.38=179,0,179" "${x},0.90!=179,0,179")
endforeach()
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/far-plane.png" --tolerance=40 ${probes}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message("ENG_TERRAIN_FAR_PLANE_FAULT")
    message(FATAL_ERROR "terrain far plane: the ground does not reach the far plane across the picture")
endif()
