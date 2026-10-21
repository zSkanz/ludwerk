# A picture of the editor, taken with no window (`--editor-shots`), and asked a
# few things.
#
# The editor's interface is drawn into a headless frame -- laid out in a
# window of the platform's offscreen kind, which is memory and nothing on any
# screen -- while a drive script moves a mouse over it; each `shot <name>` of
# the script writes `<name>.png`. What is asked of a picture is what a person
# would look for first: that the panel is there, that the playhead is where
# the ruler was pressed, that the section a selection should have is drawn.
# Flat colours of the interface itself, which no rasteriser rounds; never the
# scene behind it.
#
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DPROJECT=<project folder>
#         -DDRIVE=<file.drive> -DCHECKS=<file.checks> -DOUTPUT=<dir>
#         [-DGAME=1] [-DCAPTURE=<file>] [-DCOPY=1] [-DSAID=<words>] -P run_editor_shots.cmake
#
# `-DCOPY=1`: the drive is run on a copy of the project, made under the
# output -- for a drive that SAVES something, which would otherwise leave the
# example it was pointed at changed in the tree (ADR 0211: a graph saved with
# a line typed wrong on purpose).
#
# `-DSAID=<words>`: the editor is expected to say an error with these words
# in it, and must: a compiler's error that the picture is of. Any other error
# is still a failure.
#
# `-DMADE=<path>`: a file the drive is to have written, under the project --
# a bake's (ADR 0253), and with `-DCOPY=1`, under the copy.
#
# `-DCAPTURE=<file>`: first a run of the project with no editor and no device
# that writes a profile there (`--profile-capture`, ADR 0210) -- asked to be
# one: the format's name, its frames, the scope every frame has -- and
# `<capture>` in the drive is that file's path. (Not `@CAPTURE@`: an older
# CMake reads that, in quotes, as the variable.)
#
# `-DGAME=1`: not the editor but the game itself, with the overlay a player
# opens by its key drawn open from the first frame -- the same pictures of
# the other interface a run has.
#
# A line of the checks file is a shot's name and the probes it must pass.

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE PROJECT DRIVE CHECKS OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_editor_shots.cmake: -D${required}=... is required")
    endif()
endforeach()

# **The device taken from under the test**: a hosted runner's software
# device is sometimes removed while a host draws (`DXGI_ERROR_DEVICE_REMOVED`,
# 0x887A0005), and the host then dies inside the graphics library. There is no
# picture to ask anything of, so the test skips -- and says everything it
# knows first, the crash report included, because the runner keeps no file and
# the second sighting had no stack to read.
function(device_was_removed out)
    set(${out} FALSE PARENT_SCOPE)
    if(NOT host_output MATCHES "0x887A0005|device instance has been suspended")
        return()
    endif()
    message("${host_output}")
    string(REGEX MATCH "\\[crash\\] wrote ([^\n\r]*\\.txt)" _ "${host_output}")
    if(CMAKE_MATCH_1 AND EXISTS "${CMAKE_MATCH_1}")
        file(READ "${CMAKE_MATCH_1}" report)
        message("--- the host's crash report (${CMAKE_MATCH_1}) ---\n${report}\n--- end of it ---")
    endif()
    message("ENG_TEST_SKIP: the graphics device was removed under the test")
    set(${out} TRUE PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
if(COPY)
    # Without what a run leaves beside a project: its cache and its log.
    file(COPY "${PROJECT}/" DESTINATION "${OUTPUT}/project"
         PATTERN ".engine" EXCLUDE
         PATTERN "engine.log" EXCLUDE
         PATTERN "*.dmp" EXCLUDE)
    set(PROJECT "${OUTPUT}/project")
endif()
if(CAPTURE)
    execute_process(
        COMMAND "${HOST}" "${PROJECT}" --headless --rhi=null --pace=60 --frames=150 --exit --locale=en
                "--profile-capture=${CAPTURE}" --profile-lines
        RESULT_VARIABLE capture_result
        OUTPUT_VARIABLE capture_output
        ERROR_VARIABLE capture_output)
    if(NOT capture_result EQUAL 0 OR NOT EXISTS "${CAPTURE}")
        message("${capture_output}")
        message(FATAL_ERROR "editor shots: the run that was to write a profile exited ${capture_result} and wrote none")
    endif()
    file(READ "${CAPTURE}" capture_text)
    string(JSON capture_format ERROR_VARIABLE capture_error GET "${capture_text}" format)
    string(JSON capture_frames ERROR_VARIABLE capture_error LENGTH "${capture_text}" frames)
    string(JSON capture_root ERROR_VARIABLE capture_error GET "${capture_text}" scopes 0 name)
    if(NOT capture_format STREQUAL "profile-capture" OR NOT capture_root STREQUAL "frame" OR capture_frames LESS 100)
        message(FATAL_ERROR
            "editor shots: ${CAPTURE} is not a profile of the run: format `${capture_format}`, "
            "${capture_frames} frame(s), first scope `${capture_root}` (${capture_error})")
    endif()
    message("editor shots: a profile of ${capture_frames} frames was written to ${CAPTURE}")
    file(READ "${DRIVE}" drive_text)
    string(REPLACE "<capture>" "${CAPTURE}" drive_text "${drive_text}")
    set(DRIVE "${OUTPUT}/with-capture.drive")
    file(WRITE "${DRIVE}" "${drive_text}")
endif()
# At the lowest quality: what is asked of these pictures is the interface, and
# the world behind it is a cost -- on a runner whose device is the CPU, most of
# the minute the heaviest of them took.
set(shell --edit)
if(GAME)
    set(shell "")
endif()
execute_process(
    COMMAND "${HOST}" "${PROJECT}" ${shell} --headless --width=1600 --height=900 "--editor-drive=${DRIVE}"
            "--editor-shots=${OUTPUT}" --frames=3000 --exit --locale=en --quality=low
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0)
    device_was_removed(removed)
    if(removed)
        return()
    endif()
    message("${host_output}")
    message(FATAL_ERROR "editor shots: the host exited ${host_result}")
endif()
# The errors it said, a line each: the one that was expected is looked for,
# and whatever else it said is a failure as before.
string(REPLACE "[" "(" host_lines "${host_output}")
string(REPLACE "]" ")" host_lines "${host_lines}")
string(REPLACE ";" "," host_lines "${host_lines}")
string(REPLACE "\n" ";" host_lines "${host_lines}")
set(expected_said FALSE)
set(unexpected "")
foreach(host_line IN LISTS host_lines)
    string(FIND "${host_line}" "(error)" error_at)
    if(error_at EQUAL -1)
        continue()
    endif()
    set(words_at -1)
    if(DEFINED SAID)
        string(FIND "${host_line}" "${SAID}" words_at)
    endif()
    if(words_at EQUAL -1)
        list(APPEND unexpected "${host_line}")
    else()
        set(expected_said TRUE)
    endif()
endforeach()
if(unexpected)
    message("${host_output}")
    message(FATAL_ERROR "editor shots: the editor said an error while it was driven")
endif()
if(DEFINED SAID AND NOT expected_said)
    message("${host_output}")
    message(FATAL_ERROR "editor shots: the editor was to say an error with `${SAID}` in it, and said none")
endif()
if(DEFINED MADE AND NOT EXISTS "${PROJECT}/${MADE}")
    message("${host_output}")
    message(FATAL_ERROR "editor shots: the drive was to write ${MADE}, and nothing is there")
endif()

file(STRINGS "${CHECKS}" lines)
set(asked 0)
foreach(line IN LISTS lines)
    if(line MATCHES "^#" OR line STREQUAL "")
        continue()
    endif()
    separate_arguments(words NATIVE_COMMAND "${line}")
    list(POP_FRONT words shot)
    if(NOT EXISTS "${OUTPUT}/${shot}.png")
        message("${host_output}")
        message(FATAL_ERROR "editor shots: the drive never reached `shot ${shot}`")
    endif()
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${shot}.png" --tolerance=10 ${words}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${shot}: ${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR "editor shots: ${shot}.png is not what it should be (see above); it is in ${OUTPUT}")
    endif()
    math(EXPR asked "${asked} + 1")
endforeach()
if(asked EQUAL 0)
    message(FATAL_ERROR "editor shots: ${CHECKS} asks nothing")
endif()
message("editor shots: ${asked} picture(s) of the editor are as they should be; they are in ${OUTPUT}")
