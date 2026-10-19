# The manager killed in the middle of a trade (ADR 0238 §3): a world under a
# manager trades a sword for gold back and forth, and the manager -- the one
# process that holds the database -- is ended at a transaction a world asked
# for, before it ran or after it was on the disk and before its answer went
# back. After each, the next run counts five swords and a thousand gold.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_manager_killed.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
# The game beside `servers.toml`, as a manager finds it.
file(COPY "${PROJECT}/" DESTINATION "${WORKDIR}/game")

# **One run of the manager**, on ports of its own: a world of a run before
# may still be closing on the last one's, and nothing here waits for it.
set(run 0)
function(manage)
    cmake_parse_arguments(ARG "" "" "FLAGS" ${ARGN})
    math(EXPR door "${PORT} + ${run} * 2")
    math(EXPR world "${door} + 1")
    file(WRITE "${WORKDIR}/servers.toml"
         "[manager]\nport = ${door}\nworld_ports = \"${world}-${world}\"\n\n[[world]]\nname = \"market\"\nentry = true\n")
    file(REMOVE_RECURSE "${WORKDIR}/logs")
    execute_process(
        COMMAND "${HOST}" "--manager=${WORKDIR}/servers.toml" ${ARG_FLAGS}
        TIMEOUT 120
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    set(said "")
    if(EXISTS "${WORKDIR}/logs/market/market.log")
        file(READ "${WORKDIR}/logs/market/market.log" said)
    endif()
    set(result "${result}" PARENT_SCOPE)
    set(output "${output}\n--- the world said:\n${said}" PARENT_SCOPE)
    set(said "${said}" PARENT_SCOPE)
    math(EXPR next "${run} + 1")
    set(run ${next} PARENT_SCOPE)
endfunction()

# The first run, ended once it has counted: what there is to trade.
manage(FLAGS "--exit-on-line=[trade] swords=")
if(NOT said MATCHES "\\[trade\\] swords=5 gold=1000")
    message(FATAL_ERROR "the first run did not count five swords and a thousand gold (${result}):\n${output}")
endif()

# **Ended at a transaction, by construction**: the Nth a world asks for, a
# different one each run, half of them before it runs and half after it is
# on the disk and before the world is told.
set(killed 0)
foreach(kill RANGE 1 8)
    math(EXPR at "1 + (${kill} * 7) % 23")
    math(EXPR after "${kill} % 2")
    if(after)
        set(when "--exit-after-commit")
    else()
        set(when "")
    endif()
    manage(FLAGS "--exit-at-transaction=${at}" ${when})
    if(NOT result EQUAL 86)
        message(FATAL_ERROR "run ${kill} was to end at transaction ${at} (${result}):\n${output}")
    endif()
    if(NOT said MATCHES "\\[trade\\] swords=5 gold=1000")
        message(FATAL_ERROR "run ${kill}, before it was ended, did not count five swords and a thousand gold:\n${output}")
    endif()
    math(EXPR killed "${killed} + 1")
endforeach()

# The last count, from a run that is not ended in a trade.
manage(FLAGS "--exit-on-line=[trade] swords=")
if(NOT said MATCHES "\\[trade\\] swords=5 gold=1000")
    message(FATAL_ERROR "after ${killed} managers ended while trading, the count is wrong:\n${output}")
endif()
message("${killed} managers ended while trading, and nothing was duplicated or lost")
