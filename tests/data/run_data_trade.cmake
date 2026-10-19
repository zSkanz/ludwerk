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
# The first run, not killed: what there is to trade.
execute_process(
    COMMAND "${HOST}" "${PROJECT}" --headless "--serve=${PORT}" "--saves=${WORKDIR}/saves" --frames=60 --exit
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT output MATCHES "\\[trade\\] swords=5 gold=1000")
    message(FATAL_ERROR "the first run did not count five swords and a thousand gold (${result}):\n${output}")
endif()
set(killed 0)
# **Ended with a trade in flight, by construction**: each run is told which
# of its trades to stop at (`stopAt`), says so while that transaction has not
# come back, and `--exit-on-line` ends it at that line -- no destructor, no
# clean close, as a kill. No clock decides it: a two-core runner whose
# transactions each wait on its disk killed most runs, by time, before their
# first trade. The trade differs each run, and every run has to die so.
foreach(run RANGE 1 6)
    math(EXPR stopAt "1 + (${run} * 7) % 23")
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless "--serve=${PORT}" "--saves=${WORKDIR}/saves"
                "--scene-data={\"stopAt\":${stopAt}}" "--exit-on-line=[trade] in flight"
        TIMEOUT 120
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(output MATCHES "\\[trade\\] swords=([0-9]+) gold=([0-9]+)")
        if(NOT CMAKE_MATCH_1 EQUAL 5 OR NOT CMAKE_MATCH_2 EQUAL 1000)
            message(FATAL_ERROR "run ${run} found ${CMAKE_MATCH_1} swords and ${CMAKE_MATCH_2} gold:\n${output}")
        endif()
    endif()
    if(NOT result EQUAL 86 OR NOT output MATCHES "in flight at trade [0-9]+")
        message(FATAL_ERROR "run ${run} was to end with trade ${stopAt} in flight (${result}):\n${output}")
    endif()
    math(EXPR killed "${killed} + 1")
endforeach()
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
