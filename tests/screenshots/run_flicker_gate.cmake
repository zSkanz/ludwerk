# What temporal anti-aliasing is for, measured (ADR 0158): a fence of thin
# wires and far poles under a camera panning a third of a pixel a frame,
# drawn once for each anti-aliasing mode, a picture a frame. `imgflicker`
# measures how much the frames pop rather than change steadily:
#
#   the temporal pass alone (no sharpening) must take at least 40% of FXAA's
#   crawl away, and with the sharpening that follows it by default it must
#   still crawl less than FXAA.
#
# SMAA is reported and not held to it: it keeps an edge sharper than FXAA's
# blur, which is what it is for in a still picture, and a sharper edge pops
# more as it moves.
#
# Not a golden: it compares this machine's renders with each other, so it is
# a gate wherever there is a device.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DFLICKER=<imgflicker> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_flicker_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# Frames drawn, and the last of them measured: the history has settled by then.
set(FRAMES 72)
set(MEASURED 24)

foreach(required HOST FLICKER SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_flicker_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")

# The picture's own changes held still: no adapting exposure, no glow, no
# occlusion noise -- what moves is the camera and nothing else.
set(STILL --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows)

function(measure label mode result)
    file(MAKE_DIRECTORY "${OUTPUT}/${label}")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${FRAMES}" --exit --width=640 --height=360
                "--screenshot=${OUTPUT}/${label}/f.png" --screenshot-every=1 "--anti-aliasing=${mode}" ${STILL}
                ${ARGN}
        RESULT_VARIABLE exited
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(exited EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(${result} "" PARENT_SCOPE)
        return()
    endif()
    if(NOT exited EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "anti-aliasing flicker: the host exited ${exited} (${label})")
    endif()
    set(frames "")
    math(EXPR first "${FRAMES} - ${MEASURED}")
    math(EXPR last "${FRAMES} - 1")
    foreach(index RANGE ${first} ${last})
        string(LENGTH "${index}" digits)
        set(padded "${index}")
        while(digits LESS 3)
            set(padded "0${padded}")
            math(EXPR digits "${digits} + 1")
        endwhile()
        list(APPEND frames "${OUTPUT}/${label}/f-${padded}.png")
    endforeach()
    execute_process(
        COMMAND "${FLICKER}" ${frames}
        RESULT_VARIABLE measured
        OUTPUT_VARIABLE report
        ERROR_VARIABLE report)
    message("${label}: ${report}")
    if(NOT measured EQUAL 0)
        message(FATAL_ERROR "anti-aliasing flicker: imgflicker could not measure ${label}")
    endif()
    string(REGEX MATCH "flicker ([0-9.]+)" found "${report}")
    set(${result} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

measure(fxaa fxaa fxaa)
if("${fxaa}" STREQUAL "")
    return()
endif()
measure(smaa smaa smaa)
measure(taa taa taa --sharpness=0)
measure(taa-sharpened taa sharpened)

# CMake compares numbers as integers: in hundredths of a luma step.
foreach(name fxaa smaa taa sharpened)
    string(REGEX REPLACE "^([0-9]+)\\.([0-9][0-9]).*$" "\\1\\2" whole "${${name}}")
    set(scaled_${name} "${whole}")
endforeach()
math(EXPR taa_limit "${scaled_fxaa} * 60 / 100")
if(NOT scaled_taa LESS taa_limit)
    message(FATAL_ERROR "anti-aliasing flicker: TAA flickers ${taa}, FXAA ${fxaa} -- the temporal pass "
                        "must take at least 40% of the crawl away")
endif()
if(NOT scaled_sharpened LESS scaled_fxaa)
    message(FATAL_ERROR "anti-aliasing flicker: TAA with its sharpening flickers ${sharpened}, no less than "
                        "FXAA's ${fxaa}")
endif()
message("anti-aliasing flicker: FXAA ${fxaa}, SMAA ${smaa}, TAA ${taa}, TAA sharpened ${sharpened}")
