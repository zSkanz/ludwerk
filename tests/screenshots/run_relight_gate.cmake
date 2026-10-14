# Light that changes every frame (ADR 0203), asked of what a script saw: two
# `Dynamic` volumes of two thousand points each and a torch between them
# whose brightness is another every frame, with the frame's budget for bounced
# light at half a millisecond -- so no volume is ever lit again inside one
# frame, on any machine.
#
#   - **Both are worked out.** The near one used to be lit again the frame
#     after it finished, with the whole budget, for ever: the far one was
#     never lit at all (its progress read 0.00 nine hundred frames on).
#   - **Neither falls back from ready** while it is lit again, and
#     `GetProgress` stays 1.
#   - `Refresh` is not ready from the line that calls it, and `Completed` is
#     said again when the volume has been worked out anew.
#
# Nothing here is a clock: every frame does at least one piece of the work
# whatever the machine, so by the frame the script speaks at, both are done
# several times over.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<project> -P run_relight_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

# The host is read by the keys of what it says, not by the words (`--log-keys`).
set(ENV{ENG_LOG_KEYS} 1)
foreach(required HOST SCRIPT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_relight_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=540 --exit --locale=en --width=320 --height=180
            --global-illumination=high --global-illumination-budget=0.5 --reflections=low
            --no-bloom --no-ambient-occlusion --no-contact-shadows --no-anti-aliasing
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0)
    # As `run_split_screen_gate.cmake` has it, and for its reason.
    if(host_output MATCHES "0x887A0005|device instance has been suspended")
        message("${host_output}")
        string(REGEX MATCH "\\[crash\\] wrote ([^\n\r]*\\.txt)" _ "${host_output}")
        if(CMAKE_MATCH_1 AND EXISTS "${CMAKE_MATCH_1}")
            file(READ "${CMAKE_MATCH_1}" report)
            message("--- the host's crash report (${CMAKE_MATCH_1}) ---\n${report}\n--- end of it ---")
        endif()
        message("ENG_TEST_SKIP: the graphics device was removed under the test")
        return()
    endif()
    message("${host_output}")
    message(FATAL_ERROR "relight gate: the host exited ${host_result}")
endif()

string(REGEX MATCHALL "relight: [^\n\r]*" said "${host_output}")
foreach(line IN LISTS said)
    message("${line}")
endforeach()

foreach(volume 1 2)
    if(NOT host_output MATCHES "relight: volume ${volume} ready true progress 1 first completed at frame [1-9][0-9]* completed [1-9][0-9]* fell 0")
        message("${host_output}")
        message(FATAL_ERROR
            "relight gate: volume ${volume} was not worked out, or fell back from ready while it was lit again")
    endif()
endforeach()
if(NOT host_output MATCHES "relight: refreshed, ready at once false, completed again 1")
    message("${host_output}")
    message(FATAL_ERROR "relight gate: a refreshed volume said it was ready, or did not say it was done again once")
endif()
if(NOT host_output MATCHES "engine.info.indirect_light_relit")
    message("${host_output}")
    message(FATAL_ERROR "relight gate: no volume was lit again at all under a light that changed every frame")
endif()

message("relight: two volumes under a torch that flickers are both worked out, stay ready, and are lit again in turn")
