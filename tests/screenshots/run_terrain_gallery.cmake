# The terrain gallery (terrain audit T0): does the ground stay whole at every
# distance?
#
# **The test every earlier terrain audit lacked.** Nothing under
# `tests/screenshots` drew terrain; every defect the owner found at a distance
# -- ground that vanished, open seams between levels, steps -- was found by
# looking, never by a gate.
#
# Two runs of the gallery scene, each photographing its cases in order
# (`--screenshot-every`), both drawn with the sky magenta and the ground white
# (`--debug-view=holes`):
#
#   1. as a player sees it, every level of detail chosen by distance;
#   2. at full detail everywhere (`--terrain-detail=full`) -- the shape as it
#      was sculpted.
#
# Then `imgholes` on each picture of the first run: no sky enclosed by ground
# (a hole, an open seam), and no sky where the second run's same picture had
# ground more than a couple of pixels inside its silhouette (ground a coarse
# level lost). Every picture is checked and every count reported before the
# verdict, so one run shows the whole state of the ground.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DHOLES=<imgholes> -DSCRIPT=<project>
#         -DOUTPUT=<dir> [-DCASES=9] [-DQUALITY=high] -P run_terrain_gallery.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# Frames per case: the scene's own `FramesPerCase`.
set(FRAMES_PER_CASE 24)

foreach(required HOST HOLES SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_gallery.cmake: -D${required}=... is required")
    endif()
endforeach()
if(NOT DEFINED CASES)
    set(CASES 9)
endif()
if(NOT DEFINED QUALITY)
    set(QUALITY high)
endif()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
math(EXPR frames "${CASES} * ${FRAMES_PER_CASE}")

set(skipped OFF)
function(render name)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit
                "--screenshot=${OUTPUT}/${name}.png" "--screenshot-every=${FRAMES_PER_CASE}"
                "--quality=${QUALITY}" --debug-view=holes ${ARGN}
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
        message(FATAL_ERROR "terrain gallery: the host failed rendering ${name} (${result})")
    endif()
endfunction()

render(drawn)
if(skipped)
    return()
endif()
render(full --terrain-detail=full)

set(failed 0)
math(EXPR last "${CASES} - 1")
foreach(index RANGE 0 ${last})
    string(LENGTH "${index}" digits)
    set(padded "${index}")
    while(digits LESS 3)
        set(padded "0${padded}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(shot "${OUTPUT}/drawn-${padded}.png")
    set(reference "${OUTPUT}/full-${padded}.png")
    if(NOT EXISTS "${shot}" OR NOT EXISTS "${reference}")
        message(FATAL_ERROR "terrain gallery: case ${index} was not photographed (${shot})")
    endif()
    execute_process(
        COMMAND "${HOLES}" "${shot}" "--reference=${reference}" --slack=2
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${output}")
    if(NOT result EQUAL 0)
        math(EXPR failed "${failed} + 1")
    endif()
endforeach()

if(failed GREATER 0)
    # One line a test's expectation can match: FATAL_ERROR's text is wrapped.
    message("ENG_TERRAIN_GALLERY_FAULT: ${failed} of ${CASES}")
    message(FATAL_ERROR "terrain gallery: ${failed} of ${CASES} picture(s) show sky through or instead of the ground")
endif()
