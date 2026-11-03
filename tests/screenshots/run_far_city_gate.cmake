# The far city's gate (ADRs 0261 to 0264): the category's example, flown, and
# each of its four claims asked of the frame report.
#
# `examples/49-far-city` is copied, its world written and compiled, and the
# avenue flown at a paced sixty frames a second -- a mesh is let go ten
# seconds after nothing names it, and those are seconds of the clock. Held:
#   - the occlusion culler hid something in some window, at street level;
#   - meshes were let go, at least `MIN_LET_GO` of them;
#   - the texture budget kept some facades smaller;
#   - levels of detail changed.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DASSETC=<assetc> -DPROJECT=<example> -DSTAGE=<dir>
#         -DFRAMES=<n> -DMIN_LET_GO=<n> -P run_far_city_gate.cmake
cmake_minimum_required(VERSION 3.24)

set(ENG_NO_DEVICE_EXIT_CODE 4)
foreach(required HOST ASSETC PROJECT STAGE FRAMES MIN_LET_GO)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_far_city_gate.cmake needs -D${required}=")
    endif()
endforeach()

find_program(ENG_LUTE_EXECUTABLE lute)
if(NOT ENG_LUTE_EXECUTABLE)
    message(FATAL_ERROR "`lute` is not on PATH to write the far city's world; run scripts/bootstrap once")
endif()

file(REMOVE_RECURSE "${STAGE}")
file(COPY "${PROJECT}/" DESTINATION "${STAGE}"
    PATTERN ".engine" EXCLUDE
    PATTERN "content" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)

execute_process(
    COMMAND "${ENG_LUTE_EXECUTABLE}" "${PROJECT}/tools/generate.luau" "${STAGE}"
    # Inside the repository, where the toolchain's manifest names `lute`.
    WORKING_DIRECTORY "${PROJECT}"
    RESULT_VARIABLE generateResult
    OUTPUT_VARIABLE generateOutput
    ERROR_VARIABLE generateOutput)
if(NOT generateResult EQUAL 0)
    message(FATAL_ERROR "writing the world failed (${generateResult}):\n${generateOutput}")
endif()

file(MAKE_DIRECTORY "${STAGE}/.engine")
execute_process(
    COMMAND "${ASSETC}" --input "${STAGE}/content" --output "${STAGE}/.engine/content.lpack"
        --manifest "${STAGE}/.engine/content.manifest.json"
    RESULT_VARIABLE compileResult
    OUTPUT_VARIABLE compileOutput
    ERROR_VARIABLE compileOutput)
if(NOT compileResult EQUAL 0)
    message(FATAL_ERROR "assetc failed (${compileResult}):\n${compileOutput}")
endif()

execute_process(
    COMMAND "${HOST}" "${STAGE}" --headless --frames=${FRAMES} --exit --pace=60 --width=640 --height=360
        --frame-report=1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "far city: the flight failed (${result})")
endif()

# Hidden, in some window.
set(hidden FALSE)
string(REGEX MATCHALL "Occlusion culling: [0-9]+ of" lines "${output}")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "Occlusion culling: ([0-9]+) of" "\\1" count "${line}")
    if(count GREATER 0)
        set(hidden TRUE)
    endif()
endforeach()

# Let go, in all.
set(letGo 0)
string(REGEX MATCHALL "Let go of [0-9]+ meshes" lines "${output}")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "Let go of ([0-9]+) meshes" "\\1" count "${line}")
    math(EXPR letGo "${letGo} + ${count}")
endforeach()

# Kept smaller, in some window.
set(smaller 0)
string(REGEX MATCHALL "MiB; [0-9]+ kept smaller" lines "${output}")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "MiB; ([0-9]+) kept smaller" "\\1" count "${line}")
    if(count GREATER smaller)
        set(smaller ${count})
    endif()
endforeach()

# Levels changed, in all.
set(changes 0)
string(REGEX MATCHALL "Levels of detail: [0-9]+ changes" lines "${output}")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "Levels of detail: ([0-9]+) changes" "\\1" count "${line}")
    math(EXPR changes "${changes} + ${count}")
endforeach()

message("far city: hidden ${hidden}; ${letGo} meshes let go; up to ${smaller} facades kept smaller; ${changes} changes of level")
if(NOT hidden)
    message("${output}")
    message(FATAL_ERROR "far city: the culler hid nothing at street level")
endif()
if(letGo LESS ${MIN_LET_GO})
    message(FATAL_ERROR "far city: ${letGo} meshes let go, where at least ${MIN_LET_GO} were left behind")
endif()
if(smaller EQUAL 0)
    message(FATAL_ERROR "far city: the texture budget kept nothing smaller")
endif()
if(changes EQUAL 0)
    message(FATAL_ERROR "far city: no level of detail changed")
endif()
