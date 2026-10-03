# A slab's shadow on itself (D506): does a thin slab's top, which the sun
# meets at a low angle, stay lit at every cascade's distance?
#
# One run of the scene, photographing each case twice in order
# (`--screenshot-every`): with the sun's shadows, then with
# `Lighting.GlobalShadows` off. Nothing in the scene stands between the sun and
# a face of the slab turned to it, so the two pictures of a case should be the
# same, and `imgcmp` counts where they are not: the slab shadowing itself.
#
# **Not one pixel may differ by more than two levels**, measured: with the bias
# in metres for every cascade, fourteen of the thirty-six cases did, all past
# the near cascade; with the slope part of the bias, none. A lone slab is the
# mildest form of it: in the arena that reported it, 11% of the lit pixels in
# the second cascade and 19% in the band after it came out darkened.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DCOMPARE=<imgcmp> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_mesh_shadow.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# The scene's own `FramesPerCase`, and its picture count: two a case.
set(FRAMES_PER_CASE 12)
set(PICTURES 72)
# Two levels of rounding either way are the same picture.
set(TOLERANCE 2)
set(MAX_DIFFERENT 0)

foreach(required HOST COMPARE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_mesh_shadow.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
math(EXPR frames "${PICTURES} * ${FRAMES_PER_CASE}")

execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit "--screenshot=${OUTPUT}/slab.png"
            "--screenshot-every=${FRAMES_PER_CASE}" --width=640 --height=360
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "mesh shadow: the host failed (${result})")
endif()

function(padded index out)
    string(LENGTH "${index}" digits)
    set(text "${index}")
    while(digits LESS 3)
        set(text "0${text}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

set(failed 0)
math(EXPR last "${PICTURES} - 1")
foreach(index RANGE 0 ${last} 2)
    math(EXPR other "${index} + 1")
    padded(${index} lit)
    padded(${other} plain)
    set(shadowed "${OUTPUT}/slab-${lit}.png")
    set(unshadowed "${OUTPUT}/slab-${plain}.png")
    foreach(shot "${shadowed}" "${unshadowed}")
        if(NOT EXISTS "${shot}")
            message(FATAL_ERROR "mesh shadow: a picture was not taken (${shot})")
        endif()
    endforeach()
    execute_process(
        COMMAND "${COMPARE}" "${shadowed}" "${unshadowed}" --tolerance ${TOLERANCE}
                --max-different-pixels ${MAX_DIFFERENT} --diff "${OUTPUT}/diff-${lit}.png"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message("case ${lit}: ${output}")
        math(EXPR failed "${failed} + 1")
    endif()
endforeach()

if(failed GREATER 0)
    # One line a test's expectation can match: FATAL_ERROR's text is wrapped.
    message("ENG_MESH_SHADOW_FAULT: ${failed}")
    message(FATAL_ERROR "mesh shadow: ${failed} case(s) show a slab's top shadowed by the slab itself")
endif()
