# A trade killed in the middle (ADR 0238 §3): a server trades a sword for gold
# back and forth and is killed at a moment nobody chose, again and again;
# after each, the next run counts five swords and a thousand gold -- nothing
# duplicated, nothing lost.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_trade.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(killed 0)
foreach(run RANGE 1 8)
    # Killed after a time that differs each run, and differs from the time
    # a trade takes.
    math(EXPR seconds "2 + (${run} * 7) % 3")
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless "--serve=${PORT}" "--saves=${WORKDIR}/saves"
        TIMEOUT ${seconds}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(output MATCHES "\\[trade\\] swords=([0-9]+) gold=([0-9]+)")
        if(NOT CMAKE_MATCH_1 EQUAL 5 OR NOT CMAKE_MATCH_2 EQUAL 1000)
            message(FATAL_ERROR "run ${run} found ${CMAKE_MATCH_1} swords and ${CMAKE_MATCH_2} gold:\n${output}")
        endif()
    endif()
    if(output MATCHES "traded=[1-9]")
        math(EXPR killed "${killed} + 1")
    endif()
endforeach()
if(killed LESS 3)
    message(FATAL_ERROR "only ${killed} runs traded before they were killed")
endif()
# The last count, from a run that is not killed.
execute_process(
    COMMAND "${HOST}" "${PROJECT}" --headless "--serve=${PORT}" "--saves=${WORKDIR}/saves" --frames=5 --exit
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT output MATCHES "\\[trade\\] swords=5 gold=1000")
    message(FATAL_ERROR "after ${killed} runs killed while trading, the count is wrong:\n${output}")
endif()
message("${killed} runs killed while trading, and nothing was duplicated or lost")
