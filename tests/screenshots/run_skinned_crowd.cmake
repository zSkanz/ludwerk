# A crowd of one skinned mesh drawn as one run (H2), against the same crowd
# drawn a copy at a time.
#
# Two runs of the scene: as it draws, and with `--no-instancing`. The two
# pictures must be the same to two levels, and the first must have taken fewer
# draws than the second -- the run is the point, and a picture that matched
# because nothing was batched would prove nothing.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DCOMPARE=<imgcmp> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_skinned_crowd.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST COMPARE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_skinned_crowd.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

foreach(mode runs single)
    set(extra "")
    if(mode STREQUAL "single")
        set(extra "--no-instancing")
    endif()
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=40 --exit "--screenshot=${OUTPUT}/${mode}.png"
                --width=640 --height=360 --frame-stats ${extra}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${mode}.png")
        message("${output}")
        message(FATAL_ERROR "skinned crowd: the host failed (${result}, ${mode})")
    endif()
    # A script error or a mesh that did not load is a picture of something
    # else, however well the two runs agree.
    string(FIND "${output}" "[error]" errorAt)
    string(FIND "${output}" "[warn]" warnAt)
    if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
        message("${output}")
        message(FATAL_ERROR "skinned crowd: the run warned or failed (${mode})")
    endif()
    if(NOT output MATCHES "([0-9]+) draws for")
        message("${output}")
        message(FATAL_ERROR "skinned crowd: no frame statistics (${mode})")
    endif()
    set(draws_${mode} "${CMAKE_MATCH_1}")
endforeach()

if(NOT draws_runs LESS draws_single)
    message(FATAL_ERROR
        "skinned crowd: drawn as runs took ${draws_runs} draws, a copy at a time ${draws_single}")
endif()

execute_process(
    COMMAND "${COMPARE}" "${OUTPUT}/runs.png" "${OUTPUT}/single.png" --tolerance 2 --max-different-pixels 0
            --diff "${OUTPUT}/diff.png"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "skinned crowd: the run is not the crowd drawn a copy at a time (${OUTPUT}/diff.png)")
endif()
message("skinned crowd: ${draws_runs} draws as runs, ${draws_single} a copy at a time")
