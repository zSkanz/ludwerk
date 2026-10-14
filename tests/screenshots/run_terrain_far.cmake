# A streamed terrain drawn whole (ADR 0144): is the outline of ground far past
# the load radius the outline of the ground itself?
#
# The project is copied, and run three times:
#
#   1. With no scene, its script builds ground three kilometres across --
#      about two thousand 64 m cells, a 2 m slab with a hill, a ball and a
#      ledge -- and the field is the whole of it. Photographed from a
#      kilometre, from two and from a kilometre on the other side, in
#      `--debug-view=holes`: ground one grey, sky magenta.
#   2. Saved as the project's scene (`--save-scene`).
#   3. Run again with that scene, whose ground the partition cuts into cells
#      and the streamer loads round the camera, 300 m: all the rest is drawn
#      from the cells on disk. Photographed from the same cameras.
#
# **The two runs' pictures must be one**, pixel for pixel: the far ground is
# the same function of the same voxels. Before ADR 0144 the streamed run drew
# what was loaded and nothing past it -- from two kilometres, no ground at all,
# 154 000 of the 230 000 pixels different.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project> -DOUTPUT=<dir>
#         -P run_terrain_far.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# The scene's own `FramesPerCase`, and its case count.
set(FRAMES_PER_CASE 120)
set(CASES 3)

foreach(required HOST IMGCMP SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_far.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
set(project "${OUTPUT}/project")
file(COPY "${SCRIPT}/" DESTINATION "${project}")
math(EXPR frames "${CASES} * ${FRAMES_PER_CASE}")

function(run name)
    execute_process(
        COMMAND "${HOST}" "${project}" --headless ${ARGN}
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
        message(FATAL_ERROR "terrain far: the host failed the ${name} run (${result})")
    endif()
endfunction()

set(skipped OFF)
set(view "--frames=${frames}" --exit "--screenshot-every=${FRAMES_PER_CASE}" --debug-view=holes --width=640
         --height=360)
run(whole ${view} "--screenshot=${OUTPUT}/whole.png")
if(skipped)
    return()
endif()
run(save --frames=1 "--save-scene=${project}/content/scenes/main.scene.json")
file(READ "${project}/project.toml" toml)
string(REPLACE "engine = \"0.1\"" "engine = \"0.1\"\nscene = \"scenes/main.scene.json\"" toml "${toml}")
file(WRITE "${project}/project.toml" "${toml}")
run(streamed ${view} "--screenshot=${OUTPUT}/streamed.png")

set(failed 0)
math(EXPR last "${CASES} - 1")
foreach(index RANGE 0 ${last})
    string(LENGTH "${index}" digits)
    set(padded "${index}")
    while(digits LESS 3)
        set(padded "0${padded}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    execute_process(
        COMMAND "${IMGCMP}" "${OUTPUT}/whole-${padded}.png" "${OUTPUT}/streamed-${padded}.png" --tolerance 8
                --max-different-pixels 0 --diff "${OUTPUT}/diff-${padded}.png"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${output}")
    if(NOT result EQUAL 0)
        math(EXPR failed "${failed} + 1")
    endif()
endforeach()

if(failed GREATER 0)
    message("ENG_TERRAIN_FAR_FAULT: ${failed}")
    message(FATAL_ERROR "terrain far: ${failed} picture(s) of the streamed ground are not the whole ground's")
endif()
