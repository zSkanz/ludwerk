# A cloth's shadow does not speckle from frame to frame.
#
# The sun's shadow decides, from the four texels of the map round a lookup,
# whether a receiver is under something that touches it or in the clear, and
# compares it away from the light or towards it. The first rule that did so
# asked only whether those texels were in front of the receiver's depth -- and
# a cloth is one sheet, with no far side: what the map holds round a point of
# it is the cloth itself, in front in two of the four across a texel of its
# slope. It was read as a caster of itself, and the cloth came out with a
# speckle of dark pixels that was different in every frame.
#
# **No count over a still picture saw it**: the shadowed pixels of a frame went
# from 6,875 to 6,949, which reads as "the same". A count of pixels that POP
# did -- out of line for one frame by a few levels and back the next
# (`imgflicker --pops`): 901 more in three hundred frames, every one on the
# cloth. So a moving measure is part of what any rule here has to pass, and
# this is it kept.
#
# `examples/35-cape` -- a cloth carried and swinging -- a picture a frame,
# drawn twice: as it is, and with the sun's shadow lookup skipped
# (`--skip=shadow`). What crosses a pixel for a frame pops it too, and so does
# a rasteriser's own edge; both runs have those, and the difference is what
# the shadows add. Small pops only, 6 to 24 levels: a thing crossing a pixel
# pops it by the whole difference between the thing and what is behind it.
# Measured on the machine this was written on: 2,841 without the lookup;
# 4,104 with it under the released rule (1,263 more), 4,981 under the rule
# that speckled (2,140), 4,093 as kept (1,252): a mesh that bends keeps the
# released bias, and the cloth's own pixels are the released ones. Held at
# 1,500.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DFLICKER=<imgflicker> -DSCRIPT=<examples/35-cape> -DOUTPUT=<dir>
#         -P run_cloth_shadow_flicker_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# A picture a frame for five and a half seconds; the first half second is the
# scene arriving and is left out.
set(FRAMES 330)
set(LEFT_OUT 31)
set(MOST_ADDED 1500)

foreach(required HOST FLICKER SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_cloth_shadow_flicker_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")

function(pops_of name out)
    file(MAKE_DIRECTORY "${OUTPUT}/${name}")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --width=960 --height=540 "--frames=${FRAMES}" --exit --locale=en
                --quality=high ${ARGN} "--screenshot=${OUTPUT}/${name}/frame.png" --screenshot-every=1
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        set(${out} "skip" PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0)
        message("${host_output}")
        message(FATAL_ERROR "cloth shadow flicker: ${name}: the host exited ${host_result}")
    endif()
    file(GLOB pictures "${OUTPUT}/${name}/frame-*.png")
    list(SORT pictures)
    list(LENGTH pictures taken)
    if(taken LESS 300)
        message(FATAL_ERROR "cloth shadow flicker: ${name}: ${taken} pictures where a frame each was asked")
    endif()
    list(SUBLIST pictures ${LEFT_OUT} -1 pictures)
    execute_process(
        COMMAND "${FLICKER}" --pops=6,24 ${pictures}
        RESULT_VARIABLE flicker_result
        OUTPUT_VARIABLE flicker_output
        ERROR_VARIABLE flicker_output)
    message("${name}: ${flicker_output}")
    if(NOT flicker_result EQUAL 0 OR NOT flicker_output MATCHES "frames, ([0-9]+) pops")
        message(FATAL_ERROR "cloth shadow flicker: ${name}: the pops could not be counted")
    endif()
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

pops_of(with shadowed)
if(shadowed STREQUAL "skip")
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
pops_of(without bare --skip=shadow)

math(EXPR added "${shadowed} - ${bare}")
message("cloth shadow flicker: ${shadowed} pops with the sun's shadows, ${bare} without: the shadows add ${added}")
if(added GREATER MOST_ADDED)
    message(FATAL_ERROR
        "cloth shadow flicker: the sun's shadows add ${added} pops to a cloth in ${FRAMES} frames, over the "
        "${MOST_ADDED} they may.\n"
        "A shadow that speckles differently in every frame: a receiver is being taken for its own caster. The "
        "pictures are in ${OUTPUT}.")
endif()
message("cloth shadow flicker: a cloth's shadow is as steady as it was")
