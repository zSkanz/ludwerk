# Paint blended until it is the ground, beside paint not yet (D396): does the
# ground under the paint show through along the line between them?
#
# One picture of the scene in `--debug-view=albedo`, straight down on the
# line: sand ground on one side, grass with sand over it at nine tenths on the
# other, the layers' repeat left unbroken. Before the fix the grass showed
# through along the line, a green band whose red ran 92 levels from the sand's;
# sand alone ranges over 33. `imgsteps` holds every channel's spread to 48.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSTEPS=<imgsteps> -DSCRIPT=<project> -DOUTPUT=<dir>
#         -P run_terrain_paint_edge.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
set(MAX_SPREAD 48)

foreach(required HOST STEPS SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_paint_edge.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=20 --exit "--screenshot=${OUTPUT}/albedo.png"
            --debug-view=albedo --width=640 --height=360
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/albedo.png")
    message("${output}")
    message(FATAL_ERROR "terrain paint edge: the host failed (${result})")
endif()

execute_process(
    COMMAND "${STEPS}" "${OUTPUT}/albedo.png" --step=255 "--max-spread=${MAX_SPREAD}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message("ENG_TERRAIN_PAINT_EDGE_FAULT")
    message(FATAL_ERROR "terrain paint edge: the ground under the paint shows through where paint became the ground")
endif()
