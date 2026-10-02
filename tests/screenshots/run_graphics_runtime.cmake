# What a script changes through `GraphicsService` while the game runs is what
# starting that way would have drawn (ADR 0147) -- and a flag outranks a script.
#
# A differential, as `run_settings_differential.cmake` is and for its reason: a
# setting that is accepted, read back and reaches nothing that draws looks
# exactly like one that works, and only pixels can tell. The scene's own script
# asks for `Low` half a second in; it is drawn three ways:
#
#   no flag          the level changes at run time, by the script
#   --quality=low    `Low` from the start
#   --quality=ultra  the command line pinned it: the script's write is not in force
#
# The first two must be the SAME picture, and the third must NOT be.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -DFRAMES=<n> -P run_graphics_runtime.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST IMGCMP SCRIPT OUTPUT FRAMES)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_graphics_runtime.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

function(render out)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${FRAMES}" --exit --width=640 --height=360
                "--screenshot=${out}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${output}")
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "graphics runtime differential: the host exited ${result} (${ARGN})")
    endif()
    if(NOT EXISTS "${out}")
        message(FATAL_ERROR "graphics runtime differential: no file written (${ARGN})")
    endif()
endfunction()

render("${OUTPUT}/by-script.png")
if(NOT EXISTS "${OUTPUT}/by-script.png")
    # The skip was already printed by `render`; there is nothing to compare.
    return()
endif()
render("${OUTPUT}/from-start.png" --quality=low)
render("${OUTPUT}/pinned.png" --quality=ultra)

# The same picture, to within what a frame's adapting exposure leaves: the
# script's frame spent its first half second at another level.
execute_process(
    COMMAND "${IMGCMP}" "${OUTPUT}/by-script.png" "${OUTPUT}/from-start.png" --tolerance 3 --max-different-pixels 64
    RESULT_VARIABLE same_result
    OUTPUT_VARIABLE same_output
    ERROR_VARIABLE same_output)
message("${same_output}")
if(NOT same_result EQUAL 0)
    message(FATAL_ERROR
        "graphics runtime differential: a level set by a script while the game runs is not the picture "
        "of a game started at that level.\nA setting written through GraphicsService reaches the model "
        "and not everything that draws.")
endif()

# Inverted: a match is the failure.
execute_process(
    COMMAND "${IMGCMP}" "${OUTPUT}/by-script.png" "${OUTPUT}/pinned.png" --tolerance 2 --max-different-pixels 0
    RESULT_VARIABLE pinned_result
    OUTPUT_VARIABLE pinned_output
    ERROR_VARIABLE pinned_output)
message("${pinned_output}")
if(pinned_result EQUAL 0)
    message(FATAL_ERROR
        "graphics runtime differential: with --quality=ultra the frame is the one a script's `Low` draws.\n"
        "The command line did not outrank the script.")
endif()

message("graphics runtime differential: a script's level is the level, and a flag's stands over it")
