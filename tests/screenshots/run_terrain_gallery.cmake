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
# ground further inside its silhouette than the quality's error budget allows
# (ground a coarse level lost). A level is chosen so its shape is within the
# budget's pixels of the full one (ADR 0140), and while a node slides onto its
# parent it shows up to 1/0.85 of that -- so the slack is that, rounded up,
# with the floor below. Sky enclosed by ground has no slack at all. Every picture is checked and every count reported before the
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
# The error budget in pixels (`GraphicsSettings::terrainPixelError`: ultra
# 1.5, high 2, medium 3, low 4), over 0.85 and rounded up -- and never under
# 3: a level's error is measured over its cells, and a curved silhouette, a
# ball's top close up, moves a pixel further than that says.
if(QUALITY STREQUAL "medium")
    set(SLACK 4)
elseif(QUALITY STREQUAL "low")
    set(SLACK 5)
else()
    set(SLACK 3)
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
        COMMAND "${HOLES}" "${shot}" "--reference=${reference}" --slack=${SLACK}
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
