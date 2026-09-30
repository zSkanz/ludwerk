# Every face of a block in its block's colour (D380).
#
# A pile of red blocks from above and two sides, and `imgprobe` at a point of
# each face: red, lit, on every backend. On Vulkan the sides drew black -- the
# palette was a uniform block of 16 KiB, which SDL_GPU binds 4 KiB at a time,
# and only the tops were in the first 4 KiB. Probed rather than compared with a
# golden: the claim is about three faces' colour, not the frame.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_block_faces.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_block_faces.cmake: -D${required}=... is required")
    endif()
endforeach()

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=30 --exit "--screenshot=${OUTPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message("${output}")
    message(FATAL_ERROR "block faces: the host failed (${result})")
endif()

# The left side, the right side and the top, as drawn with the palette whole.
# The tolerance takes a rasteriser's rounding and a software one's lighting;
# black is two hundred away.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40 "0.42,0.6=179,60,78" "0.58,0.6=174,57,75" "0.5,0.35=235,95,111"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message("ENG_BLOCK_FACES_FAULT")
    message(FATAL_ERROR "block faces: a face of a block is not its block's colour")
endif()
