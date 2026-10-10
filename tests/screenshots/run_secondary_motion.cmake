# A cape's chains are found and stepped in a real run (ADR 0194).
#
# The solver has its own tests on numbers. What this holds is everything
# between a `SpringBone` in a scene and the solver: the instance found under
# its mesh, the chain gathered from the rig, the frame it is stepped in, and
# the pose handed to the picture -- none of which a unit test of the solver
# touches, and all of which could be wrong while it passed.
#
# Usage:
#   cmake -DHOST=<engine-host> -DSCRIPT=<examples/35-cape> -P run_secondary_motion.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST SCRIPT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_secondary_motion.cmake: -D${required}=... is required")
    endif()
endforeach()

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=100 --exit --pace=60 --frame-report=1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "secondary motion: the host failed (${result})")
endif()
string(FIND "${output}" "[error]" errorAt)
string(FIND "${output}" "[warn]" warnAt)
if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "secondary motion: the run warned or failed")
endif()

string(REGEX MATCH "Secondary motion: ([0-9]+) chains of ([0-9]+) joints" found "${output}")
if(found STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "secondary motion: no chain was stepped in a scene with six")
endif()
message("secondary motion: ${CMAKE_MATCH_1} chains of ${CMAKE_MATCH_2} joints")
if(NOT CMAKE_MATCH_1 EQUAL 6 OR NOT CMAKE_MATCH_2 EQUAL 30)
    message(FATAL_ERROR "secondary motion: two figures of three columns of five joints are 6 chains of 30 joints")
endif()
