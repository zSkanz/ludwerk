# A game learns how its last run ended, and where its report is (ADR 0187).
#
# Six runs of the host in a row, against one log, each the "run before" of the
# next. `says` prints `RunService:GetLastRun()` and ends; `fails` raises an
# error nothing catches and ends; `--fault-on-purpose` dies of a real fault
# once the crash handler is in.
#
#   1. says            -> None: nothing came before.
#   2. fails           (ends when asked to, with one script error)
#   3. says            -> Clean, one script error, its text, and a report that
#                         holds it.
#   4. says, faulting  (dies; the handler writes its note)
#   5. says            -> Crashed, and a report that holds the handler's note.
#   6. says            -> Clean, no errors, and no report: the crash's is gone.
#
# What only this can check: that the record a run leaves, the note the crash
# handler writes and the log's rotation -- three files, written by three
# pieces of the engine -- are found by the next run under the names it looks
# for. Each has unit tests; a crash cannot be checked from inside the process
# that has it.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECTS=<tests/lastrun> -DSCRATCH=<dir>
#         -P run_last_run_gate.cmake

foreach(required HOST PROJECTS SCRATCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_last_run_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}")

# One run. `said` is the line `says` printed, or empty.
function(run project said_var result_var)
    execute_process(
        COMMAND "${HOST}" "${PROJECTS}/${project}" --headless --rhi=null --frames=120 "--log-file=${SCRATCH}/engine.log" ${ARGN}
        WORKING_DIRECTORY "${SCRATCH}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output
        TIMEOUT 120)
    string(REGEX MATCH "LASTRUN [^\r\n]*" said "${output}")
    set(${said_var} "${said}" PARENT_SCOPE)
    set(${result_var} "${result}" PARENT_SCOPE)
    set(last_output "${output}" PARENT_SCOPE)
endfunction()

function(expect said pattern what)
    if(NOT said MATCHES "${pattern}")
        message(FATAL_ERROR "${what}\n  it said: ${said}\n--- output ---\n${last_output}")
    endif()
endfunction()

# 1. Nothing came before.
run(says said result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "The first run did not end well (exit ${result}):\n${last_output}")
endif()
expect("${said}" "outcome=None errors=0 first=none report=none dump=none" "A first run has a run before it.")

# 2. An error nothing catches, and a clean end.
run(fails said result)

# 3. Clean, with the error and a report of it.
run(says said result)
expect("${said}" "outcome=Clean errors=1 first=[^\r\n]*the shop could not be opened"
    "A run with a script error nothing caught was not told of as one.")
string(REGEX MATCH "report=([^ ]+)" matched "${said}")
set(report "${CMAKE_MATCH_1}")
if(report STREQUAL "none" OR NOT EXISTS "${report}")
    message(FATAL_ERROR "A run with a script error left no report: ${said}")
endif()
file(READ "${report}" text)
if(NOT text MATCHES "the shop could not be opened" OR NOT text MATCHES "Ludwerk Last Run 1.2.0"
   OR NOT text MATCHES "script errors nothing caught: 1")
    message(FATAL_ERROR "The report does not say what the run was and what went wrong in it:\n${text}")
endif()

# 4. A fault.
run(says said result --fault-on-purpose)
if(result EQUAL 0)
    message(FATAL_ERROR "A run asked to fault ended well:\n${last_output}")
endif()

# 5. Crashed, with the handler's note in the report.
run(says said result)
expect("${said}" "outcome=Crashed" "A run that died of a fault was not told of as a crash.")
string(REGEX MATCH "report=([^ ]+)" matched "${said}")
set(report "${CMAKE_MATCH_1}")
if(report STREQUAL "none" OR NOT EXISTS "${report}")
    message(FATAL_ERROR "A run that crashed left no report: ${said}")
endif()
file(READ "${report}" text)
if(NOT text MATCHES "The crash note" OR NOT text MATCHES "crashed")
    message(FATAL_ERROR "The report of a crash does not hold the crash handler's note:\n${text}")
endif()
if(WIN32 AND NOT said MATCHES "dump=yes")
    message(FATAL_ERROR "A crash on Windows leaves a dump, and the next run was not told of one: ${said}")
endif()

# 6. And that report is that run's alone.
run(says said result)
expect("${said}" "outcome=Clean errors=0 first=none report=none dump=none"
    "A run that ended well was told of with the crash before it.")
if(EXISTS "${report}")
    message(FATAL_ERROR "The report of a crash is still there after a run that ended well: ${report}")
endif()

message(STATUS "A game is told how its last run ended: none, a script error, a crash, and clean again.")
