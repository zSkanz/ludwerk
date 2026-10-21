# The lean path draws a window's world in one pass, and draws it as the full
# path does (the mobile lean path).
#
# `tests/screenshots/specular` at Medium, at a fixed exposure, twice: as a
# computer draws it, and with `--lean` -- the picture a phone draws at Low and
# Medium. Held:
#   - lean, the world is drawn in the forward pass with samples, and nothing a
#     tile GPU would leave its tile for: no depth prepass, no bloom, no
#     anti-aliasing pass -- and, the exposure fixed, no tonemap pass either,
#     the tonemap folded into the forward pass;
#   - the two pictures are the same picture: within `ENG_LEAN_TOLERANCE` of
#     each other but for `ENG_LEAN_PIXELS` pixels, the edges the samples smooth
#     and the bloom the lean path leaves out.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_lean_path.cmake
set(ENG_NO_DEVICE_EXIT_CODE 4)
set(ENG_LEAN_TOLERANCE 24)
# A fraction of a percent of the frame.
set(ENG_LEAN_PIXELS 1200)
foreach(required HOST IMGCMP SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_lean_path.cmake: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/game"
    PATTERN ".engine" EXCLUDE
    PATTERN "engine.log" EXCLUDE
    PATTERN "engine.previous.log" EXCLUDE)

function(draw name)
    execute_process(
        COMMAND "${HOST}" "${OUTPUT}/game" --headless --frames=120 --exit --quality=medium --no-auto-exposure
                --gpu-pass-times --frame-report=1 --width=640 --height=360 "--screenshot=${OUTPUT}/${name}.png" ${ARGN}
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
        message(FATAL_ERROR "lean path: the ${name} run failed (${result})")
    endif()
    # The last report's passes.
    string(REGEX MATCHALL "GPU milliseconds by pass[^\n]*" reports "${output}")
    list(POP_BACK reports passes)
    set(${name}_passes "${passes}" PARENT_SCOPE)
endfunction()

draw(full --no-lean)
if(skipped)
    return()
endif()
draw(lean --lean)

if(NOT lean_passes MATCHES "forward/forward-samples")
    message(FATAL_ERROR "lean path: the forward pass did not draw with samples: ${lean_passes}")
endif()
foreach(left depth-prepass bloom/bloom-down smaa/ tonemap)
    string(FIND "${lean_passes}" "${left}" at)
    if(NOT at EQUAL -1)
        message(FATAL_ERROR "lean path: `${left}` was drawn on the lean path: ${lean_passes}")
    endif()
endforeach()
# And the full path still has them, so the test says something.
if(NOT full_passes MATCHES "tonemap" OR NOT full_passes MATCHES "bloom/bloom-down")
    message(FATAL_ERROR "lean path: the full path did not draw what it draws: ${full_passes}")
endif()

execute_process(
    COMMAND "${IMGCMP}" "${OUTPUT}/lean.png" "${OUTPUT}/full.png" --tolerance ${ENG_LEAN_TOLERANCE}
            --max-different-pixels ${ENG_LEAN_PIXELS} --diff "${OUTPUT}/difference.png"
    RESULT_VARIABLE compared
    OUTPUT_VARIABLE said
    ERROR_VARIABLE said)
if(NOT compared EQUAL 0)
    message("${said}")
    message(FATAL_ERROR "lean path: the lean picture is not the full one (see ${OUTPUT}/difference.png)")
endif()
message("lean path: one pass with samples, the tonemap folded in; the picture the full path's -- ${said}")
