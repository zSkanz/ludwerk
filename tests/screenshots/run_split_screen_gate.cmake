# Several players at one screen (ADR 0205), asked of pictures of
# `examples/41-couch` -- which a scene's data stands still in each way of
# sharing the window, with the line between views magenta and eight pixels
# wide so a picture can be asked where it is.
#
# What each picture holds is a flat colour of the window's own making, never
# the world behind it, so it is the same on every rasteriser:
#
#   columns   two views side by side: the line down the middle and nowhere
#             else; each player's own interface at the top of that player's
#             half, in that player's colour;
#   grid      three views in the window's quarters: the fourth is black, the
#             lines are a cross, and three interfaces each in their quarter;
#   together  `Dynamic` with the two players near each other: one picture --
#             no line at all;
#   apart     the same with them forty metres apart, left and right: the line
#             upright through the middle;
#   side      that frame again through `--debug-view=side`, which shows the
#             second player's view whole: the half it is not seen on is BLACK
#             -- nothing was lit there -- and the half it is seen on is not.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DPROJECT=<examples/41-couch>
#         -DOUTPUT=<dir> -P run_split_screen_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE PROJECT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_split_screen_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")

# **The device taken from under the test**: a hosted runner's software
# device is sometimes removed while a host draws (`DXGI_ERROR_DEVICE_REMOVED`,
# 0x887A0005), and the host then dies inside the graphics library. There is no
# picture to ask anything of, so the test skips -- and says everything it
# knows first, the crash report included, because the runner keeps no file and
# the second sighting had no stack to read.
function(device_was_removed out)
    set(${out} FALSE PARENT_SCOPE)
    if(NOT host_output MATCHES "0x887A0005|device instance has been suspended")
        return()
    endif()
    message("${host_output}")
    string(REGEX MATCH "\\[crash\\] wrote ([^\n\r]*\\.txt)" _ "${host_output}")
    if(CMAKE_MATCH_1 AND EXISTS "${CMAKE_MATCH_1}")
        file(READ "${CMAKE_MATCH_1}" report)
        message("--- the host's crash report (${CMAKE_MATCH_1}) ---\n${report}\n--- end of it ---")
    endif()
    message("ENG_TEST_SKIP: the graphics device was removed under the test")
    set(${out} TRUE PARENT_SCOPE)
endfunction()

# One picture: `name`, the scene's data, any flags of the host's, then probes.
# Sixteen frames of a window 1280 by 720: what is asked is there from the first,
# and on a runner whose device is the CPU five pictures of a larger one took
# seven minutes, beside which other tests' clocks ran out.
function(picture name data flags)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless --frames=16 --exit --locale=en --width=1280 --height=720
                "--screenshot=${OUTPUT}/${name}.png" "--scene-data=${data}" ${flags}
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0)
        device_was_removed(removed)
        if(removed)
            set(skipped TRUE PARENT_SCOPE)
            return()
        endif()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${name}.png")
        message("${host_output}")
        message(FATAL_ERROR "split screen gate: the host exited ${host_result} and wrote no picture of `${name}`")
    endif()
    if(host_output MATCHES "\\[error\\]|\\[warn\\]")
        message("${host_output}")
        message(FATAL_ERROR "split screen gate: the host warned or erred drawing `${name}`")
    endif()
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${name}.png" --tolerance=12 ${ARGN}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${name}: ${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR "split screen gate: `${name}` is not as it should be (see above); it is in ${OUTPUT}")
    endif()
endfunction()

set(skipped FALSE)
set(magenta "255,0,255")
# The seats' colours, which each one's label is filled with.
set(red "236,92,80")
set(blue "84,156,240")
set(yellow "246,204,76")

# Side by side. The line: down the middle at three heights, and not a tenth
# of the window to either side of it. The labels, a corner a seat: the first
# player's in the top left of the left half, the second's in the top right of
# the right half -- and not in the top right of the left one, where it would
# be were it laid out in the window.
picture(columns "{\"players\":2,\"mode\":\"Columns\",\"apart\":30,\"line\":true}" ""
    "0.5000,0.2000=${magenta}" "0.5000,0.5000=${magenta}" "0.5000,0.8000=${magenta}"
    "0.4000,0.5000!=${magenta}" "0.6000,0.5000!=${magenta}"
    "0.0190,0.0280=${red}" "0.9810,0.0280=${blue}" "0.5190,0.0280!=${blue}")
if(skipped)
    return()
endif()

# The quarters. Nobody's is black; the lines are a cross; an interface each,
# half the size in half the height: the first's top left, the second's top
# right of the top right quarter, the third's low in the bottom left quarter.
picture(grid "{\"players\":3,\"mode\":\"Grid\",\"apart\":30,\"line\":true}" ""
    "0.7500,0.7500=0,0,0" "0.9000,0.9000=0,0,0"
    "0.5000,0.2500=${magenta}" "0.2500,0.5000=${magenta}" "0.7500,0.5000=${magenta}" "0.5000,0.7500=${magenta}"
    "0.2500,0.2500!=${magenta}" "0.2500,0.2500!=0,0,0"
    "0.0110,0.0160=${red}" "0.4890,0.0160!=${blue}" "0.9890,0.0160=${blue}" "0.0110,0.9500=${yellow}")

if(skipped)
    return()
endif()

# Together: one picture. Nothing along where a line would be.
picture(together "{\"players\":2,\"mode\":\"Dynamic\",\"apart\":10,\"line\":true}" ""
    "0.5000,0.2000!=${magenta}" "0.5000,0.5000!=${magenta}" "0.5000,0.8000!=${magenta}")

if(skipped)
    return()
endif()

# Apart, left and right: the line upright through the middle.
picture(apart "{\"players\":2,\"mode\":\"Dynamic\",\"apart\":44,\"line\":true}" ""
    "0.5000,0.2000=${magenta}" "0.5000,0.5000=${magenta}" "0.5000,0.8000=${magenta}"
    "0.4000,0.5000!=${magenta}" "0.6000,0.5000!=${magenta}")

if(skipped)
    return()
endif()

# And the second player's view whole. It is seen on the left: the right of it
# was never drawn -- no surface, no sky -- and every pixel of it is black from
# a little past the line to the window's edge (between the second player's
# label at the top and the caption at the bottom, which are interface and
# drawn over it); the left is the world.
picture(side "{\"players\":2,\"mode\":\"Dynamic\",\"apart\":44,\"line\":true}" "--debug-view=side"
    "0.5300,0.0800:0.9900,0.8800~0,0,0"
    "0.2500,0.5000!=0,0,0" "0.4500,0.3000!=0,0,0")
if(skipped)
    return()
endif()

message("split screen: two side by side, three in quarters, one picture together, a line apart, and a view lit only on its side")
