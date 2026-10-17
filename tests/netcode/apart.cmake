# **One process of a pipeline, its output kept apart.**
#
# `execute_process` with several `COMMAND`s runs them at once and joins them
# as a shell pipeline does: each one's output is the next one's input. The
# harnesses here use that to run a server and its clients side by side, and
# no engine reads its input -- so what a server printed went into a pipe
# nobody emptied, and once the pipe was full the server stopped at its next
# line, mid-tick, for as long as the run lasted. Its clients were extrapolated
# through the stall and corrected after it. Measured with a server that
# printed 150 lines at once: a 17-second frame.
#
# So every process but the last runs through this script, which writes its
# output to a file and sends nothing on; the last one's output still reaches
# the harness. What a test reads is each machine's own log, as before.
#
# Invoked as:
#   cmake -DOUT=<file> -P apart.cmake -- <program> [<argument>...]
#
# `OUT` is relative to the working directory the harness gives the pipeline;
# the error output goes beside it, in `<OUT>.err`.

if(NOT DEFINED OUT)
    message(FATAL_ERROR "apart.cmake: -DOUT=<file> is required")
endif()

set(command "")
set(after FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(index RANGE ${last})
    if(after)
        list(APPEND command "${CMAKE_ARGV${index}}")
    elseif("${CMAKE_ARGV${index}}" STREQUAL "--")
        set(after TRUE)
    endif()
endforeach()
if(command STREQUAL "")
    message(FATAL_ERROR "apart.cmake: no program after --")
endif()

execute_process(
    COMMAND ${command}
    OUTPUT_FILE "${OUT}"
    ERROR_FILE "${OUT}.err"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "apart.cmake: ${command} ended with ${result}; its output is in ${OUT}")
endif()
