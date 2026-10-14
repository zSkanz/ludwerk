# A wall broken, looked at (ADR 0215).
#
# `examples/45-breaking-wall` twice: untouched, and three seconds after a heavy
# ball is rolled at it. What is held:
#
#   - the ball broke it: the example says how many joints gave, ten or more;
#   - where the ball struck, the wall is gone and what was behind it shows --
#     brighter there than on the wall's face;
#   - away from the blow the wall stands as it stood: that part of the two
#     pictures is the same, so the wall broke where it was hit and did not
#     fall over whole;
#   - nothing warned.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DPROJECT=<examples/45-breaking-wall>
#         -DOUTPUT=<dir> -P run_breaking_wall_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
foreach(required HOST PROBE PROJECT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_breaking_wall_gate.cmake: -D${required}=... is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUTPUT}")

function(shoot name data)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless --frames=200 --exit --pace=60 --width=640 --height=360 --locale=en
                "--screenshot=${OUTPUT}/${name}.png" ${data}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT result EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "breaking wall gate: the host failed (${result})")
    endif()
    string(FIND "${output}" "[error]" errorAt)
    string(FIND "${output}" "[warn]" warnAt)
    if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
        message("${output}")
        message(FATAL_ERROR "breaking wall gate: the ${name} run warned or failed")
    endif()
    set(said "${output}" PARENT_SCOPE)
endfunction()

set(skipped FALSE)
shoot(intact "")
if(skipped)
    return()
endif()
shoot(broken "--scene-data={\"roll\":true}")
string(REGEX MATCH "broke ([0-9]+) joints" _ "${said}")
if(CMAKE_MATCH_1 STREQUAL "" OR CMAKE_MATCH_1 LESS 10)
    message("${said}")
    message(FATAL_ERROR "breaking wall gate: the ball broke '${CMAKE_MATCH_1}' joints")
endif()
message("breaking wall gate: broke ${CMAKE_MATCH_1} joints")

execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/broken.png" --tolerance=24 "--against=${OUTPUT}/intact.png"
            "0.47,0.57>0.6,0.45" "0.58,0.38:0.62,0.46=="
    RESULT_VARIABLE probed
    OUTPUT_VARIABLE probeSaid
    ERROR_VARIABLE probeSaid)
message("${probeSaid}")
if(NOT probed EQUAL 0)
    message(FATAL_ERROR "breaking wall gate: the wall is not broken where it was hit; the pictures are in ${OUTPUT}")
endif()
