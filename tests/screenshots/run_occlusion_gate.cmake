# What a wall hides is not drawn, and nothing it hides is seen (ADR 0262).
#
# `tests/screenshots/occlusion`: streets of houses, a crowd of small things
# behind them, and a camera turning fast at a crossing -- a corner sweeps past
# in a few frames. `tests/screenshots/occlusion_ground`: a ridge of terrain and
# a wall of blocks, things behind each, and a camera that turns and rises over
# their lines. Drawn twice, with the occlusion culler and without it, a
# picture every fourth frame. Held:
#   - every picture of the two runs is the same, pixel for pixel: a thing
#     hidden wrongly, or one appearing a frame late as the camera turns, is a
#     difference;
#   - the culler hid something, so the test says something.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_occlusion_gate.cmake
set(ENG_NO_DEVICE_EXIT_CODE 4)
foreach(required HOST IMGCMP SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_occlusion_gate.cmake: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}/culled" "${OUTPUT}/whole")
file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/game"
    PATTERN ".engine" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)

function(draw name)
    execute_process(
        COMMAND "${HOST}" "${OUTPUT}/game" --headless --frames=120 --exit --width=640 --height=360 --frame-report=1
                "--screenshot=${OUTPUT}/${name}/shot.png" --screenshot-every=4 ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT result EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "occlusion: the ${name} run failed (${result})")
    endif()
    set(${name}_output "${output}" PARENT_SCOPE)
endfunction()

draw(culled)
if(skipped)
    return()
endif()
draw(whole --no-occlusion-culling)

string(REGEX MATCHALL "Occlusion culling: ([0-9]+) of" hid "${culled_output}")
set(anything FALSE)
foreach(line IN LISTS hid)
    string(REGEX REPLACE "Occlusion culling: ([0-9]+) of" "\\1" count "${line}")
    if(count GREATER 0)
        set(anything TRUE)
    endif()
endforeach()
if(NOT anything)
    message("${culled_output}")
    message(FATAL_ERROR "occlusion: the culler hid nothing")
endif()

file(GLOB pictures RELATIVE "${OUTPUT}/culled" "${OUTPUT}/culled/*.png")
list(LENGTH pictures count)
if(count LESS 20)
    message(FATAL_ERROR "occlusion: ${count} pictures, where thirty were asked for")
endif()
foreach(picture IN LISTS pictures)
    execute_process(
        COMMAND "${IMGCMP}" "${OUTPUT}/culled/${picture}" "${OUTPUT}/whole/${picture}" --tolerance 0
                --max-different-pixels 0 --diff "${OUTPUT}/difference-${picture}"
        RESULT_VARIABLE same
        OUTPUT_VARIABLE said
        ERROR_VARIABLE said)
    if(NOT same EQUAL 0)
        message("${said}")
        message(FATAL_ERROR "occlusion: ${picture} differs with the culler (see ${OUTPUT}/difference-${picture})")
    endif()
endforeach()
message("occlusion: ${count} pictures the same with and without the culler, which hid what was behind")
