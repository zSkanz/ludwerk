# What a temporal upscaler is for, measured (ADR 0164): a scene of edges at
# every angle and bars a pixel or two across, under a camera that does not
# move, drawn at the output's resolution and at HALF of it, upscaled each way.
# `imgcmp` counts the pixels each upscale leaves visibly unlike the first:
#
#   FSR 2, which has every frame before this one to build from, must leave
#   fewer than three fifths of what FSR 1 leaves, which has this frame alone;
#   and at a render scale of 1, where it is the anti-aliasing and nothing
#   else, fewer than it leaves from half.
#
# Not a golden: it compares this machine's renders with each other, so it is
# a gate wherever there is a device with compute shaders -- and where there is
# none the engine says so and upscales by FSR 1, which is reported as a skip.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_upscale_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# Forty frames: at half the resolution the jitter's sequence is thirty-two
# long, and a history has seen all of it.
set(FRAMES 40)
# A channel off by more than this is a pixel visibly unlike the reference: an
# edge in another place, a bar that is not there.
set(TOLERANCE 16)

foreach(required HOST IMGCMP SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_upscale_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

# The picture's own changes held still: no adapting exposure, no glow, no
# occlusion noise, and no sharpening after any of them -- what differs is the
# upscale and nothing else.
set(STILL --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows --sharpness=0)

set(skipped FALSE)
function(render label)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${FRAMES}" --exit --width=640 --height=360
                "--screenshot=${OUTPUT}/${label}.png" ${STILL} ${ARGN}
        RESULT_VARIABLE exited
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(exited EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT exited EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "temporal upscale: the host exited ${exited} (${label})")
    endif()
    # The engine's own words for a device that cannot run FSR 2
    # (`render.warn.fsr2_unavailable`).
    if(output MATCHES "FSR 2 was asked for and this device cannot run it")
        message("ENG_TEST_SKIP: this device has no compute shaders for FSR 2")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT EXISTS "${OUTPUT}/${label}.png")
        message(FATAL_ERROR "temporal upscale: no picture written (${label})")
    endif()
endfunction()

function(unlike label result)
    execute_process(
        COMMAND "${IMGCMP}" "${OUTPUT}/native.png" "${OUTPUT}/${label}.png" --tolerance ${TOLERANCE}
                --max-different-pixels 0
        OUTPUT_VARIABLE report
        ERROR_VARIABLE report)
    string(REGEX MATCH "([0-9]+) differing pixel" found "${report}")
    if("${CMAKE_MATCH_1}" STREQUAL "")
        message("${report}")
        message(FATAL_ERROR "temporal upscale: imgcmp could not compare ${label}")
    endif()
    set(${result} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# The reference: the output's resolution, its edges settled over the frames.
render(native --anti-aliasing=taa)
if(skipped)
    return()
endif()
render(fsr2-half --upscaling=fsr2 --render-scale=0.5)
if(skipped)
    return()
endif()
render(fsr2-native --upscaling=fsr2)
render(fsr1-half --upscaling=fsr1 --render-scale=0.5 --anti-aliasing=smaa)

unlike(fsr2-half fsr2)
unlike(fsr2-native whole)
unlike(fsr1-half fsr1)
message("temporal upscale: of 230400 pixels, unlike the picture at full resolution -- FSR 2 from half ${fsr2}, "
        "FSR 1 from half ${fsr1}, FSR 2 at full ${whole}")

math(EXPR limit "${fsr1} * 60 / 100")
if(NOT fsr2 LESS limit)
    message(FATAL_ERROR "temporal upscale: FSR 2 from half the resolution leaves ${fsr2} pixels unlike the "
                        "full picture and FSR 1 leaves ${fsr1} -- the frames before this one must put back "
                        "at least two fifths of what one frame cannot")
endif()
if(NOT whole LESS fsr2)
    message(FATAL_ERROR "temporal upscale: FSR 2 at a render scale of 1 leaves ${whole} pixels unlike the "
                        "full picture, no fewer than the ${fsr2} it leaves from half of it")
endif()
