# A start that fails leaves a trace, and a log begins at the beginning (D608).
#
# Two runs of the host, neither with a window or a graphics device.
#
# One is asked for an option it does not have, as a shortcut with a mistyped
# flag asks: it must refuse (exit code 2) -- and, because nothing in its
# arguments says "no window", it is taken for a game somebody opened, whose
# failure has to be findable afterwards. It ended before any log was open, so
# what it said goes to a file in the start folder: the first line saying what
# ran and what it was asked, and the refusal. That folder is the system's
# temporary one, pointed here at a folder of the test's own.
#
# The other runs a scene for three frames with a log file named: the log's
# first line must be that same line of identity -- written before the
# catalogue, the options and the project were read -- and not the first thing
# the engine happened to say once it knew where its log went.
#
# What is not held here: the dialog. It is shown only where nobody reads the
# output, and a test reads the output.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<scene> -DOUTPUT=<dir> -P run_start_trace.cmake

foreach(required HOST SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_start_trace.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}/tmp")
file(TO_NATIVE_PATH "${OUTPUT}/tmp" temporary)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "TEMP=${temporary}" "TMP=${temporary}" "TMPDIR=${temporary}"
            "${HOST}" --no-such-option
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 2)
    message("${output}")
    message(FATAL_ERROR "start trace: an option that does not exist answered ${result}, not a usage error")
endif()
file(GLOB started "${OUTPUT}/tmp/*-start/start-*.log")
list(LENGTH started count)
if(NOT count EQUAL 1)
    message("${output}")
    message(FATAL_ERROR "start trace: a start that failed before its log left ${count} start logs, not one")
endif()
file(READ "${started}" said)
# The first line: what ran, and what it was asked.
if(NOT said MATCHES "^[^\n]*\\| [0-9]+ \\|[^\n]*--no-such-option")
    message("${said}")
    message(FATAL_ERROR "start trace: the start log does not begin with what ran and what it was asked")
endif()
if(NOT said MATCHES "\\[error\\][^\n]*--no-such-option")
    message("${said}")
    message(FATAL_ERROR "start trace: the start log does not have the refusal in it")
endif()

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --rhi=null --frames=3 --exit "--log-file=${OUTPUT}/run.log"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/run.log")
    message("${output}")
    message(FATAL_ERROR "start trace: the ordinary run failed (${result})")
endif()
file(READ "${OUTPUT}/run.log" logged)
if(NOT logged MATCHES "^[^\n]*\\| [0-9]+ \\|[^\n]*--frames=3")
    message("${logged}")
    message(FATAL_ERROR "start trace: the log does not begin with what ran and what it was asked")
endif()
# And it is not printed: a tool that reads this program's output reads what it
# asked for.
if(output MATCHES "\\| [0-9]+ \\|[^\n]*--frames=3")
    message("${output}")
    message(FATAL_ERROR "start trace: the line of identity was printed as well as logged")
endif()
message("start trace: a failed start left ${started}; the log begins at the beginning")
