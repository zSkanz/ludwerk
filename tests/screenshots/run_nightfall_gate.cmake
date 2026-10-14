# What a sequence puts over the picture (ADR 0204), asked of two pictures of
# `examples/42-nightfall`, whose sequence fades in from black over a second
# and a half behind the bars of a wider picture:
#
#   opening   eight frames in: the picture is still all but black;
#   playing   two hundred frames in: the fade is gone and the bars are there
#             -- black across the top and across the bottom of the window,
#             and not across its middle.
#
# And the example says what it is: three shots, twenty seconds, known from
# the line that set its `Sequence`.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DPROJECT=<examples/42-nightfall>
#         -DOUTPUT=<dir> -P run_nightfall_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE PROJECT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_nightfall_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")

function(picture name frames)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless "--frames=${frames}" --exit --locale=en --width=320 --height=180
                "--screenshot=${OUTPUT}/${name}.png" --no-auto-exposure --no-bloom --no-anti-aliasing
                --global-illumination=low --reflections=low
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0 AND host_output MATCHES "0x887A0005|device instance has been suspended")
        # As `run_split_screen_gate.cmake` has it, and for its reason.
        message("${host_output}")
        message("ENG_TEST_SKIP: the graphics device was removed under the test")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${name}.png")
        message("${host_output}")
        message(FATAL_ERROR "nightfall gate: the host exited ${host_result} and wrote no picture of `${name}`")
    endif()
    if(host_output MATCHES "\\[error\\]|\\[warn\\]")
        message("${host_output}")
        message(FATAL_ERROR "nightfall gate: the host warned or erred")
    endif()
    if(NOT host_output MATCHES "\\[nightfall\\] 3 shots, 20 seconds")
        message("${host_output}")
        message(FATAL_ERROR "nightfall gate: the sequence did not say its three shots and its twenty seconds")
    endif()
endfunction()

function(ask name)
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${name}.png" ${ARGN}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${name}: ${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR "nightfall gate: `${name}` is not as it should be (see above); it is in ${OUTPUT}")
    endif()
endfunction()

set(skipped FALSE)
picture(opening 8)
if(skipped)
    return()
endif()
ask(opening --tolerance=60 "0.500,0.500=0,0,0" "0.250,0.300=0,0,0")

picture(playing 200)
if(skipped)
    return()
endif()
ask(playing --tolerance=6 "0.300,0.030=0,0,0" "0.700,0.030=0,0,0" "0.300,0.975=0,0,0" "0.700,0.975=0,0,0")
ask(playing --tolerance=60 "0.500,0.500!=0,0,0" "0.250,0.300!=0,0,0")

message("nightfall: the picture comes up from black behind the bars of a wider one")
