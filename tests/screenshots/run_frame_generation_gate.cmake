# What a made frame has to be, measured (ADR 0165): a scene whose every motion
# the game knows -- a sliding camera, a box crossing, a bar turning -- drawn
# twice.
#
#   With frame generation, time moves two steps a frame, and a frame is made
#   between every two drawn. Without, it moves one step a frame: the frame
#   made between steps 2n and 2n + 2 has a frame DRAWN at step 2n + 1 to be
#   held against.
#
# `imgcmp` counts the pixels of each made frame that are grossly unlike the
# drawn one, and the pixels of the nearer of its two neighbours that are --
# what the window would have shown without it. Over ten made frames:
#
#   the made frames must be wrong in fewer than three fifths of the pixels
#   their neighbours are;
#   and every frame that was DRAWN in the first run must be the frame drawn at
#   that step in the second, which is what says the two runs are the same
#   scene and a made frame went in between, not in place.
#
# Then, on Windows, with a window: the same scene for ninety frames, which
# must show frames it made -- the path a window takes is not the one a
# picture read back takes (the made frame is shown first, and the drawn one
# when the next frame's drawing begins).
#
# Not a golden: it compares this machine's renders with each other, so it is
# a gate wherever there is a device that can generate frames -- and where
# there is none the engine says so and shows frames as drawn, which is
# reported as a skip.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project>
#         -DOUTPUT=<dir> [-DWINDOW=ON] -P run_frame_generation_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# Sixteen frames drawn with a frame made between them, thirty-two without.
set(FRAMES 16)
# A channel off by more than this is a thing in the wrong place, not an edge
# a little softer than it was drawn.
set(TOLERANCE 32)

foreach(required HOST IMGCMP SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_frame_generation_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}/made" "${OUTPUT}/drawn")

# The picture's own changes held still: no adapting exposure and no glow --
# what differs between two frames is where things are.
set(STILL --no-auto-exposure --no-bloom)

function(run directory frames)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit --width=640 --height=360
                "--screenshot=${OUTPUT}/${directory}/f.png" --screenshot-every=1 ${STILL} ${ARGN}
        RESULT_VARIABLE exited
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    set(run_output "${output}" PARENT_SCOPE)
    set(run_exited "${exited}" PARENT_SCOPE)
endfunction()

run(made ${FRAMES} --frame-generation)
if(run_exited EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT run_exited EQUAL 0)
    message("${run_output}")
    message(FATAL_ERROR "frame generation: the host exited ${run_exited}")
endif()
# The engine's own words for a device that cannot (`render.warn.frame_generation_*`).
if(run_output MATCHES "Frame generation was asked for")
    message("ENG_TEST_SKIP: this device cannot generate frames")
    return()
endif()
if(NOT run_output MATCHES "Frame generation showed ([0-9]+) more frames")
    message("${run_output}")
    message(FATAL_ERROR "frame generation: asked for, not refused, and no frame was made")
endif()
set(made_count "${CMAKE_MATCH_1}")

math(EXPR twice "${FRAMES} * 2")
run(drawn ${twice})
if(NOT run_exited EQUAL 0)
    message("${run_output}")
    message(FATAL_ERROR "frame generation: the host exited ${run_exited} without it")
endif()

function(name index result)
    set(padded "${index}")
    string(LENGTH "${padded}" digits)
    while(digits LESS 3)
        set(padded "0${padded}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(${result} "f-${padded}.png" PARENT_SCOPE)
endfunction()

function(unlike first second result)
    execute_process(
        COMMAND "${IMGCMP}" "${first}" "${second}" --tolerance ${TOLERANCE} --max-different-pixels 0
        OUTPUT_VARIABLE report
        ERROR_VARIABLE report)
    string(REGEX MATCH "([0-9]+) differing pixel" found "${report}")
    if("${CMAKE_MATCH_1}" STREQUAL "")
        message("${report}")
        message(FATAL_ERROR "frame generation: imgcmp could not compare ${first} with ${second}")
    endif()
    set(${result} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# The first run shows, after its first frame, a made frame and then a drawn
# one: picture 2k is made and picture 2k + 1 drawn. The second run's picture
# n + 1 is the scene one step on from its picture n, and the first run's
# picture n is the scene at the step of the second's picture n + 1.
set(made_total 0)
set(near_total 0)
foreach(shown RANGE 10 28 2)
    math(EXPR truth "${shown} + 1")
    math(EXPR after "${shown} + 2")
    name(${shown} made_name)
    name(${shown} before_name)
    name(${truth} truth_name)
    name(${after} after_name)
    unlike("${OUTPUT}/drawn/${truth_name}" "${OUTPUT}/made/${made_name}" made)
    unlike("${OUTPUT}/drawn/${truth_name}" "${OUTPUT}/drawn/${before_name}" before)
    unlike("${OUTPUT}/drawn/${truth_name}" "${OUTPUT}/drawn/${after_name}" behind)
    set(near ${before})
    if(behind LESS near)
        set(near ${behind})
    endif()
    math(EXPR made_total "${made_total} + ${made}")
    math(EXPR near_total "${near_total} + ${near}")

    # And the drawn frame after it is the frame drawn at that step.
    math(EXPR drawn_shown "${shown} + 1")
    name(${drawn_shown} drawn_name)
    name(${after} same_name)
    execute_process(
        COMMAND "${IMGCMP}" "${OUTPUT}/drawn/${same_name}" "${OUTPUT}/made/${drawn_name}" --tolerance 2
                --max-different-pixels 16
        RESULT_VARIABLE same
        OUTPUT_VARIABLE same_report
        ERROR_VARIABLE same_report)
    if(NOT same EQUAL 0)
        message("${same_report}")
        message(FATAL_ERROR "frame generation: the frame DRAWN after a made one (${drawn_name}) is not the frame "
                            "drawn at that step without it -- a made frame took a drawn one's place, or the two "
                            "runs are not one scene")
    endif()
endforeach()
message("frame generation: ${made_count} frames made; over ten of them, grossly wrong pixels of 230400 -- the "
        "made frames ${made_total}, the nearer drawn neighbour ${near_total}")
math(EXPR limit "${near_total} * 60 / 100")
if(NOT made_total LESS limit)
    message(FATAL_ERROR "frame generation: the made frames are wrong in ${made_total} pixels where showing the "
                        "nearer drawn frame again is wrong in ${near_total} -- a made frame must put back at "
                        "least two fifths of that")
endif()

if(WINDOW)
    execute_process(
        # Behind whatever started it, as every test's window opens.
        COMMAND "${HOST}" "${SCRIPT}" --frames=90 --exit --width=640 --height=360 --frame-generation
                --max-frame-rate=60 --background-frame-rate=0 ${STILL}
        RESULT_VARIABLE exited
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(exited EQUAL ENG_NO_DEVICE_EXIT_CODE)
        return()
    endif()
    if(NOT exited EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "frame generation: with a window, the host exited ${exited}")
    endif()
    if(NOT output MATCHES "Frame generation showed ([0-9]+) more frames")
        message("${output}")
        message(FATAL_ERROR "frame generation: with a window, no frame was made")
    endif()
    if(CMAKE_MATCH_1 LESS 60)
        message("${output}")
        message(FATAL_ERROR "frame generation: with a window, ${CMAKE_MATCH_1} frames made of ninety drawn")
    endif()
    message("frame generation: with a window, ${CMAKE_MATCH_1} frames made of ninety drawn")
endif()
