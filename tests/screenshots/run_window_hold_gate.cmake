# A window the system holds does not stop the engine (D535).
#
# On Windows a title bar held, a border dragged or a window's menu open is a
# loop the system runs itself, inside the call that asks it for events. The
# main loop stood still for as long as the hand did: a host that moved its
# window froze the match for everyone in it.
#
# The host is asked to hold its own window for two seconds
# (`--simulate-window-hold`: the same notices to the window and the same loop
# inside the event pump, with no hand), in a real window on a real device, and
# no frame of the run may take anything like that long. Before the fix one
# frame took 1992 ms; with it the longest is the first, some tens.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCENE=<windowhold> -P run_window_hold_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# A quarter of the hold: far over any frame of a part and a camera, far under
# a loop that stood still.
set(LONGEST_MS 500)

foreach(required HOST SCENE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_window_hold_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

execute_process(
    # Sixty frames a second whether or not anybody is looking at the window: a
    # test's opens behind whatever started it.
    COMMAND "${HOST}" "${SCENE}" --frames=360 --exit --frame-stats --simulate-window-hold=120
            --max-frame-rate=60 --background-frame-rate=0 --width=640 --height=360
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0)
    message("${host_output}")
    message(FATAL_ERROR "a held window: the host exited ${host_result}")
endif()
string(REGEX MATCH "measured frames: median ([0-9.]+) ms, worst ([0-9.]+) ms" found "${host_output}")
if(found STREQUAL "")
    message("${host_output}")
    message(FATAL_ERROR "a held window: the run printed no frame statistics")
endif()
set(worst "${CMAKE_MATCH_2}")
message("held for two seconds: the longest frame ${worst} ms (at most ${LONGEST_MS}), the median ${CMAKE_MATCH_1} ms")
if(worst GREATER LONGEST_MS)
    message(FATAL_ERROR "a held window: a frame took ${worst} ms -- the loop stood still while the window was held")
endif()
