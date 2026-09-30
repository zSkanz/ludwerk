# The terrain's shadow on itself, and the bend of its normal (terrain audit
# T3): does ground the sun faces stay lit, and does its shading draw a crease
# the ground does not have?
#
# Two runs of the scene, each photographing its cases in order
# (`--screenshot-every`):
#
#   1. `--debug-view=shadow`: the map in red, the contact mask in green, blue
#      where the ground faces the sun. Nothing in the scene stands between the
#      sun and a face turned to it, so `imgshadow` counts the faces either
#      shadow darkens: the ground shadowing itself.
#   2. `--debug-view=bend`: how far the shading bends the mesh's normal. The
#      ground has no material, so the bend is the grain's alone, noise with no
#      crease; `imgsteps` counts where it jumps between two pixels.
#
# **The map may darken a few pixels a picture**: along the terminator of the
# smallest ball, a triangle's edge crosses a pixel whose centre is on a face
# turned to the sun and whose filter reads the face beside it, turned away.
# Sixty-six at most, measured; the old push darkened up to 180 000 of a
# picture's 500 000. **The contact mask a handful**, sixteen at most, where a
# ray leaves a ball's rim; with a bias that ignored the depth buffer's slant it
# darkened 5 600 of the plain. Nothing steps.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSHADOW=<imgshadow> -DSTEPS=<imgsteps>
#         -DSCRIPT=<project> -DOUTPUT=<dir> -P run_terrain_shadow.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# The scene's own `FramesPerCase`, and its case count.
set(FRAMES_PER_CASE 24)
set(CASES 88)
set(MAX_MAP 150)
set(MAX_CONTACT 48)

foreach(required HOST SHADOW STEPS SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_shadow.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
math(EXPR frames "${CASES} * ${FRAMES_PER_CASE}")

set(skipped OFF)
function(render name view)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit
                "--screenshot=${OUTPUT}/${name}.png" "--screenshot-every=${FRAMES_PER_CASE}"
                "--debug-view=${view}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped ON PARENT_SCOPE)
        return()
    endif()
    if(NOT result EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "terrain shadow: the host failed rendering ${name} (${result})")
    endif()
endfunction()

render(shadow shadow)
if(skipped)
    return()
endif()
render(bend bend)

set(failed 0)
math(EXPR last "${CASES} - 1")
foreach(index RANGE 0 ${last})
    string(LENGTH "${index}" digits)
    set(padded "${index}")
    while(digits LESS 3)
        set(padded "0${padded}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    foreach(pair "shadow;${SHADOW};--max-map=${MAX_MAP};--max-contact=${MAX_CONTACT}" "bend;${STEPS};--max=0")
        list(GET pair 0 name)
        list(GET pair 1 tool)
        list(SUBLIST pair 2 -1 limits)
        set(shot "${OUTPUT}/${name}-${padded}.png")
        if(NOT EXISTS "${shot}")
            message(FATAL_ERROR "terrain shadow: case ${index} was not photographed (${shot})")
        endif()
        execute_process(
            COMMAND "${tool}" "${shot}" ${limits}
            RESULT_VARIABLE result
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output)
        if(NOT result EQUAL 0)
            message("${output}")
            math(EXPR failed "${failed} + 1")
        endif()
    endforeach()
endforeach()

if(failed GREATER 0)
    # One line a test's expectation can match: FATAL_ERROR's text is wrapped.
    message("ENG_TERRAIN_SHADOW_FAULT: ${failed}")
    message(FATAL_ERROR "terrain shadow: ${failed} picture(s) show ground shadowing itself or a crease in its shading")
endif()
