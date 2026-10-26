# The mesh flight's gate (ADR 0264): what the card holds stays flat across a
# world whose every cell wears a mesh no other does.
#
# A strip of cells is written (`meshflight/tools/generate.luau`), compiled
# with `assetc`, and flown along its whole length at sixty frames a second --
# paced, because a mesh is let go ten seconds after nothing names it, and
# those are seconds of the clock. Held, from the frame report's lines:
#   - meshes were let go, at least `MIN_LET_GO` of them: a flight that let
#     nothing go has the flattest memory of all only if it loaded nothing;
#   - the GPU memory in buffers at the end is within `GROWTH_MB` of what it
#     was at `SETTLED` of the way along. Without letting go it grows by every
#     cell's mesh: about a third of a mebibyte each.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DASSETC=<assetc> -DPROJECT=<meshflight> -DSTAGE=<dir>
#         -DFRAMES=<n> -DMIN_LET_GO=<n> -DGROWTH_MB=<f> -P run_mesh_flight_gate.cmake
cmake_minimum_required(VERSION 3.24)

set(ENG_NO_DEVICE_EXIT_CODE 4)
foreach(required HOST ASSETC PROJECT STAGE FRAMES MIN_LET_GO GROWTH_MB)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_mesh_flight_gate.cmake needs -D${required}=")
    endif()
endforeach()

find_program(ENG_LUTE_EXECUTABLE lute)
if(NOT ENG_LUTE_EXECUTABLE)
    message(FATAL_ERROR "`lute` is not on PATH to write the mesh flight's world; run scripts/bootstrap once")
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
    COMMAND "${HOST}" "${STAGE}" --headless --frames=${FRAMES} --exit --pace=60 --width=320 --height=180
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
    message(FATAL_ERROR "mesh flight: the run failed (${result})")
endif()

# What was let go, line by line.
set(letGo 0)
string(REGEX MATCHALL "Let go of [0-9]+ meshes" lines "${output}")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "Let go of ([0-9]+) meshes" "\\1" count "${line}")
    math(EXPR letGo "${letGo} + ${count}")
endforeach()

# The buffers on the card, a report a second.
string(REGEX MATCHALL "MiB in [0-9]+ textures and [0-9.]+ MiB in [0-9]+ buffers" memory "${output}")
list(LENGTH memory reports)
if(reports LESS 10)
    message("${output}")
    message(FATAL_ERROR "mesh flight: ${reports} memory reports, too few to judge")
endif()
set(buffers "")
foreach(line IN LISTS memory)
    string(REGEX REPLACE ".* and ([0-9.]+) MiB in [0-9]+ buffers" "\\1" mib "${line}")
    list(APPEND buffers "${mib}")
endforeach()
math(EXPR settledAt "${reports} * 2 / 5")
list(GET buffers ${settledAt} settled)
list(GET buffers -1 last)
message("mesh flight: ${letGo} meshes let go; buffers ${settled} MiB at report ${settledAt} of ${reports}, ${last} MiB at the end")

if(letGo LESS ${MIN_LET_GO})
    message("${output}")
    message(FATAL_ERROR "mesh flight: ${letGo} meshes let go, where at least ${MIN_LET_GO} were left behind")
endif()
# CMake has no floating point: compared in hundredths of a mebibyte.
foreach(name settled last GROWTH_MB)
    string(REGEX REPLACE "^([0-9]+)$" "\\1.0" value "${${name}}")
    string(REGEX MATCH "^([0-9]+)\\.([0-9]?)([0-9]?)" parts "${value}")
    set(whole "${CMAKE_MATCH_1}")
    set(tenths "${CMAKE_MATCH_2}")
    set(hundredths "${CMAKE_MATCH_3}")
    if(tenths STREQUAL "")
        set(tenths 0)
    endif()
    if(hundredths STREQUAL "")
        set(hundredths 0)
    endif()
    math(EXPR ${name}Hundredths "${whole} * 100 + ${tenths} * 10 + ${hundredths}")
endforeach()
math(EXPR ceiling "${settledHundredths} + ${GROWTH_MBHundredths}")
if(lastHundredths GREATER ceiling)
    message(FATAL_ERROR "mesh flight: the buffers grew from ${settled} MiB to ${last} MiB, more than ${GROWTH_MB}")
endif()
