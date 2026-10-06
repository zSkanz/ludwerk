# A part drawn in an instanced run is lit as the same part drawn alone (D577).
#
# Six cubes in a row, each turned an eighth of a turn about the vertical, the
# sun low on the right: five of one material, drawn as one instanced run, and
# the fourth from the left of another, a draw of its own. On every one the
# face turned to the sun -- the right one -- is the bright one. The instanced
# shaders built their normal matrix transposed, which turned an instanced
# part's normals by the inverse of its rotation: on the five the right face
# was as dark as the left, and the face in the sun was on the far side.
#
# Asked as "brighter than", face against face on the same cube, and not as
# colours: the claim is which way a normal points, whatever the light, the
# tone curve and a software rasteriser make of the numbers.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<png> -P run_instanced_normals.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_instanced_normals.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "instanced normals: the host failed (${result})")
endif()

# The cube alone first -- the picture is of what it should be -- then the
# first, third and last of the run: right face against left, at the height of
# the cubes' middles.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=60
            "0.579,0.5>0.533,0.5"
            "0.241,0.5>0.195,0.5" "0.467,0.5>0.421,0.5" "0.805,0.5>0.759,0.5"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "instanced normals: a cube's face turned to the sun is not its bright one")
endif()
