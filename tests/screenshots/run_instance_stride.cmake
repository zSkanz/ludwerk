# Every part of an instanced run drawn where it is (D484).
#
# A run of grey cubes, a blue ball and a green cube drawn one at a time, then
# a run of red cubes on the cube mesh and the instance stream the grey run
# bound; `imgprobe` at three of the red cubes. On D3D12 the vertex buffer
# views kept the last single draw's stride for the instance stream -- zero --
# so the whole red row was drawn where its first cube is, and not one of the
# three probed was there. Probed rather than compared with a golden: the claim
# is that the cubes are drawn, not how the frame looks.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_instance_stride.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_instance_stride.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "instance stride: the host failed (${result})")
endif()

# The second, fifth and last red cube. The sky behind them is pale blue, two
# hundred away in green and blue; the tolerance takes a software rasteriser's
# lighting.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=40 "0.31,0.58=167,47,66" "0.53,0.58=167,48,66" "0.76,0.58=167,48,66"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "instance stride: a cube of the instanced run is not where it was put")
endif()
