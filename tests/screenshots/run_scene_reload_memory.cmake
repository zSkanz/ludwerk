# A scene loaded again and again keeps no more than one of itself.
#
# A field whose code writes and paints a large ground and makes four hundred
# parts, loaded thirty times, half a second apart. Every component a destroyed
# instance had stayed in its pool -- a terrain's field, a script's source,
# a part's lists -- because the pools were never closed up, and a game grew by
# what a scene held each time it changed scene: this field, eleven megabytes a
# load, three hundred over the run.
#
# What the process holds the game reads at each load (`DebugService:GetStat(
# "ProcessMemoryMB")`), and the one at the sixth load and the one half a
# second after the thirtieth are within `ENG_RELOAD_GROWTH_MIB` of each other:
# what a load leaves is let go by the next, and the pools are closed up behind
# it.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_scene_reload_memory.cmake
set(ENG_NO_DEVICE_EXIT_CODE 4)
# Forty megabytes: the allocator's and the driver's own drift over a run, with
# room to spare, and an eighth of what the run grew by when every load kept
# its ground.
set(ENG_RELOAD_GROWTH_MIB 40)
foreach(required HOST SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_scene_reload_memory.cmake: -D${required}=... is required")
    endif()
endforeach()
# A copy, without whatever cache a run from the source tree left.
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/game"
    PATTERN ".engine" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)
execute_process(
    COMMAND "${HOST}" "${OUTPUT}/game" --headless --frames=1300 --exit --pace=60
            --width=320 --height=180
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "scene reload: the host failed (${result})")
endif()
string(FIND "${output}" "[error]" errorAt)
string(FIND "${output}" "[warn]" warnAt)
if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "scene reload: the run warned or failed")
endif()

# What the process held as the game said it: the game asks the system at
# each load and prints it on the line that names the load, so the reading is
# taken at the same moments every run, however long a frame takes.
function(held_at mark into)
    string(REGEX MATCH "${mark} ([0-9]+)(\\.[0-9]+)? MiB" found "${output}")
    if(found STREQUAL "")
        message("${output}")
        message(FATAL_ERROR "scene reload: the game never said \"${mark}\" with what it held")
    endif()
    set(${into} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

held_at("SCENE field 6" early)
held_at("SCENE field end" late)
math(EXPR grown "${late} - ${early}")
message("scene reload: ${early} MiB at the sixth load, ${late} MiB after the thirtieth (${grown} MiB)")
if(grown GREATER ENG_RELOAD_GROWTH_MIB)
    message(FATAL_ERROR
        "scene reload: the process grew by ${grown} MiB over 24 loads of the same scene, past "
        "${ENG_RELOAD_GROWTH_MIB}: something a load made is kept after the scene that made it is gone")
endif()
