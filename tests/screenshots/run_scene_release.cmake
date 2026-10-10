# A scene that is left takes what it alone held with it (D610).
#
# A game that goes lobby > map a > lobby > map b > lobby, each map with two
# materials' pictures and a model of its own. Every mesh and texture a game
# had ever shown used to stay on the graphics card until the engine closed:
# in the second lobby it held map a, in the third both maps -- and a game of
# six maps held all six.
#
# The frame report's memory line counts what is on the card. Read between the
# lines the game prints as it changes scene:
#   - a map holds more textures and more buffers than the lobby did;
#   - **every lobby holds what the first lobby held**, to the texture and to
#     the buffer;
#   - and the engine says so each time, once a map is left.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_scene_release.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_scene_release.cmake: -D${required}=... is required")
    endif()
endforeach()

# A copy, without whatever cache a run from the source tree left (D604).
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/game"
    PATTERN ".engine" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)

execute_process(
    COMMAND "${HOST}" "${OUTPUT}/game" --headless --frames=680 --exit --pace=60 --frame-report=1
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
    message(FATAL_ERROR "scene release: the host failed (${result})")
endif()
string(FIND "${output}" "[error]" errorAt)
string(FIND "${output}" "[warn]" warnAt)
if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "scene release: the run warned or failed")
endif()

# What the card held at the end of each stretch: the last memory line between
# one "SCENE" line and the next.
set(marks "SCENE lobby 0" "SCENE a 1" "SCENE lobby 2" "SCENE b 3" "SCENE lobby 4" "SCENE end")
set(names first_lobby map_a second_lobby map_b third_lobby)
foreach(index RANGE 0 4)
    math(EXPR after "${index} + 1")
    list(GET marks ${index} from)
    list(GET marks ${after} to)
    list(GET names ${index} name)
    string(FIND "${output}" "${from}" begin)
    string(FIND "${output}" "${to}" end)
    if(begin EQUAL -1 OR end EQUAL -1 OR NOT end GREATER begin)
        message("${output}")
        message(FATAL_ERROR "scene release: the game did not get from \"${from}\" to \"${to}\"")
    endif()
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${output}" ${begin} ${length} stretch)
    string(REGEX MATCHALL "MiB in [0-9]+ textures and [0-9.]+ MiB in [0-9]+ buffers" lines "${stretch}")
    list(LENGTH lines count)
    if(count EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "scene release: no memory line while in ${name}")
    endif()
    list(GET lines -1 last)
    string(REGEX MATCH "in ([0-9]+) textures and [0-9.]+ MiB in ([0-9]+) buffers" found "${last}")
    set(textures_${name} "${CMAKE_MATCH_1}")
    set(buffers_${name} "${CMAKE_MATCH_2}")
    message("scene release: ${name} -- ${textures_${name}} textures, ${buffers_${name}} buffers")
endforeach()

foreach(map map_a map_b)
    if(NOT textures_${map} GREATER textures_first_lobby OR NOT buffers_${map} GREATER buffers_first_lobby)
        message(FATAL_ERROR "scene release: ${map} holds no more than the lobby did, so there is nothing to let go")
    endif()
endforeach()
foreach(lobby second_lobby third_lobby)
    if(NOT textures_${lobby} EQUAL textures_first_lobby OR NOT buffers_${lobby} EQUAL buffers_first_lobby)
        message(FATAL_ERROR
            "scene release: the ${lobby} holds ${textures_${lobby}} textures and ${buffers_${lobby}} buffers, and "
            "the first lobby held ${textures_first_lobby} and ${buffers_first_lobby}: what a map alone held is "
            "still on the card")
    endif()
endforeach()

string(REGEX MATCHALL "A scene was left: [0-9]+ mesh" said "${output}")
list(LENGTH said times)
if(times LESS 2)
    message(FATAL_ERROR "scene release: two maps were left and the engine said so ${times} time(s)")
endif()
