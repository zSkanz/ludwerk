# A cape that is cloth, in a real run (ADR 0214).
#
# The soft body and the frame's rows have tests of their own on numbers
# (`soft_body_tests.cpp`, `cloth_tests.cpp`). What this holds is everything
# around them, in `examples/46-cloth-cape`: the `Cloth` found under its mesh in
# a scene, the mesh's skin kept for it, the body's colliders and the wall found,
# its rows drawn -- and the two promises a cloth makes to a picture:
#
#   - **it is where it fits**: three capes of 252 vertices each are stepped at
#     the High level, and none at Low, where each is drawn as its animation
#     has it;
#   - **it does not flicker**: a picture a frame for five seconds, and the
#     pixels that pop -- out of line for one frame by a few levels and back
#     the next (`imgflicker --pops`) -- **counted on the capes alone**: the
#     pixels that are the capes' red in the frame before or in the frame
#     (`--only=r,40`). A change to the shadows, the occlusion or the
#     anti-aliasing elsewhere in the picture can neither pass this nor fail
#     it; a cloth whose vertices went back and forth would pop the pixels it
#     covers.
#
# Measured on the machine this was written on: the capes cover about 770
# pixels a frame; 1,036 of their pixel-frames popped in 297 frames, under
# half of one per cent, the motion of two capes fluttering in gusts and one on
# a wall; 55 with the capes rigid, which stand still. Its vertices go back and
# forth by less than a millimetre from one frame to the next
# (`cloth_tests.cpp`), and a pixel here is a centimetre. Held at
# `MOST_CAPE_POPS`, a quarter above. The whole picture's count is printed for
# reading and holds nothing: 7,337 with the cloth and 3,367 without, the
# difference the edges and the shadows of capes that move where rigid ones
# stand still -- 439 of it when the two runs are made again with the sun's
# shadow, the occlusion and the contact shadows set aside
# (`--skip=shadow,occlusion,contact`, and `cloth` for the second).
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DFLICKER=<imgflicker> -DPROJECT=<examples/46-cloth-cape> -DOUTPUT=<dir>
#         -P run_cloth_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# A picture a frame for five and a half seconds; the first half second is the
# scene arriving and is left out.
set(FRAMES 330)
set(LEFT_OUT 31)
set(MOST_CAPE_POPS 1300)

foreach(required HOST FLICKER PROJECT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_cloth_gate.cmake: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

# --- Where it fits ------------------------------------------------------------

function(report quality out)
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless --frames=100 --exit --pace=60 --frame-report=1 --width=320
                --height=180 --locale=en "--quality=${quality}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        set(${out} "skip" PARENT_SCOPE)
        return()
    endif()
    if(NOT result EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "cloth gate: the ${quality} run failed (${result})")
    endif()
    string(FIND "${output}" "[error]" errorAt)
    string(FIND "${output}" "[warn]" warnAt)
    if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
        message("${output}")
        message(FATAL_ERROR "cloth gate: the ${quality} run warned or failed")
    endif()
    # The last report, not the first: the first is of a scene whose meshes
    # may not be read yet.
    string(REGEX MATCHALL "Cloth: [0-9]+ cloths of [0-9]+ vertices stepped, [0-9.]+ ms" reports "${output}")
    if(reports STREQUAL "")
        set(${out} "" PARENT_SCOPE)
    else()
        list(GET reports -1 last)
        set(${out} "${last}" PARENT_SCOPE)
    endif()
endfunction()

report(high stepped)
if(stepped STREQUAL "skip")
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
message("cloth gate: high: ${stepped}")
string(REGEX MATCH "Cloth: ([0-9]+) cloths of ([0-9]+) vertices" _ "${stepped}")
if(NOT CMAKE_MATCH_1 EQUAL 3 OR NOT CMAKE_MATCH_2 EQUAL 756)
    message(FATAL_ERROR "cloth gate: three figures with a cape of 252 vertices each are 3 cloths of 756 vertices")
endif()
report(low stepped)
if(NOT stepped STREQUAL "")
    message(FATAL_ERROR "cloth gate: at the Low level a cape is its animation's, and the run said: ${stepped}")
endif()
message("cloth gate: low: no cloth stepped")

# --- It does not flicker ------------------------------------------------------

function(pops_of name out)
    file(MAKE_DIRECTORY "${OUTPUT}/${name}")
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless --width=960 --height=540 "--frames=${FRAMES}" --exit --pace=60
                --locale=en --quality=high ${ARGN} "--screenshot=${OUTPUT}/${name}/frame.png" --screenshot-every=1
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(NOT host_result EQUAL 0)
        message("${host_output}")
        message(FATAL_ERROR "cloth gate: ${name}: the host exited ${host_result}")
    endif()
    # **By name, from their folder**: three hundred frames by their whole
    # path are past what one command line on Windows holds once the build
    # root is a few characters longer than the default, and the tool then
    # never starts.
    file(GLOB pictures RELATIVE "${OUTPUT}/${name}" "${OUTPUT}/${name}/frame-*.png")
    list(SORT pictures)
    list(LENGTH pictures taken)
    if(taken LESS 300)
        message(FATAL_ERROR "cloth gate: ${name}: ${taken} pictures where a frame each was asked")
    endif()
    list(SUBLIST pictures ${LEFT_OUT} -1 pictures)
    foreach(which whole cape)
        set(only "")
        if(which STREQUAL "cape")
            set(only "--only=r,40")
        endif()
        execute_process(
            COMMAND "${FLICKER}" --pops=6,24 ${only} ${pictures}
            WORKING_DIRECTORY "${OUTPUT}/${name}"
            RESULT_VARIABLE flicker_result
            OUTPUT_VARIABLE flicker_output
            ERROR_VARIABLE flicker_output)
        if(NOT flicker_result EQUAL 0 OR NOT flicker_output MATCHES "frames, ([0-9]+) pops")
            message("${flicker_output}")
            message(FATAL_ERROR "cloth gate: ${name}: the pops could not be counted")
        endif()
        set(${out}_${which} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endforeach()
endfunction()

pops_of(cloth hanging)
pops_of(rigid still --skip=cloth)
message("cloth gate: the whole picture, for reading: ${hanging_whole} pops with the cloth, ${still_whole} with the "
        "capes rigid")
message("cloth gate: on the capes: ${hanging_cape} pops with the cloth, ${still_cape} with the capes rigid")
if(hanging_cape GREATER MOST_CAPE_POPS)
    message(FATAL_ERROR
        "cloth gate: the capes' own pixels popped ${hanging_cape} times in ${FRAMES} frames, over the "
        "${MOST_CAPE_POPS} they may.\n"
        "A cloth whose vertices go back and forth from one frame to the next. The pictures are in ${OUTPUT}.")
endif()
message("cloth gate: the cloth moves and does not flicker")
