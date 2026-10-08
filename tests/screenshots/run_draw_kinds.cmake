# The frame report's draws, split by what each was for, add up to the report's
# own total -- and name the kinds a scene of single cubes and an instanced run
# under the sun must have.
#
# The split is what a slow match is read by: "3,300 draws" does not say
# whether the horde went four times into the sun's cascades or the effects
# stopped batching. A kind counted outside the total, or a call counted under
# none, would make every such reading wrong by an amount nobody could see.
#
# Usage:
#   cmake -DHOST=<engine-host> -DSCRIPT=<scene> -P run_draw_kinds.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST SCRIPT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_draw_kinds.cmake: -D${required}=... is required")
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
    message(FATAL_ERROR "draw kinds: the host failed (${result})")
endif()

# A report and the split that follows it. Matched in one piece and never put
# in a list: the report's own text has semicolons, which is what a list is cut
# at.
string(REGEX MATCH " ([0-9]+) draws,[^\n]*\n[^\n]*by what each was for: ([a-z_0-9, ]*)" found "${output}")
if(found STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "draw kinds: no frame report with its split in the log")
endif()
set(total "${CMAKE_MATCH_1}")
set(kinds "${CMAKE_MATCH_2}")
message("draw kinds: ${total} draws -- ${kinds}")

set(sum 0)
string(REGEX MATCHALL "[a-z_]+ [0-9]+" entries "${kinds}")
foreach(entry IN LISTS entries)
    string(REGEX MATCH "([a-z_]+) ([0-9]+)" ignored "${entry}")
    math(EXPR sum "${sum} + ${CMAKE_MATCH_2}")
    set(kind_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
endforeach()
if(NOT sum EQUAL total)
    message(FATAL_ERROR "draw kinds: the kinds add up to ${sum}, and the report says ${total} draws")
endif()
foreach(kind mesh mesh_run sun_shadow)
    if(NOT DEFINED kind_${kind})
        message(FATAL_ERROR "draw kinds: nothing was counted as ${kind}, in a scene that has it")
    endif()
endforeach()
