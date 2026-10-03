# A ViewportFrame draws with its own camera when the world has none (G7).
#
# A red crate in a ViewportFrame in the middle of the screen, and nothing in
# the world -- no `Workspace.CurrentCamera`. `imgprobe` at the crate. The views
# were drawn only when the world had a camera, and the frame was empty.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_viewport_no_camera.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_viewport_no_camera.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "viewport without camera: the host failed (${result})")
endif()

# The crate's face: red, lit by the frame's own light. The tolerance takes a
# software rasteriser's lighting; the empty frame is no red at all.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=60 "0.5,0.5=200,30,30"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "viewport without camera: the frame did not draw what is inside it")
endif()
