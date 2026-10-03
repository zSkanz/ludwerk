# What the anti-aliasing and the upscale leave alone (ADR 0158), as the engines
# that scale only their 3D do:
#
#   ui      the screen's UI is drawn after the world is resolved and scaled, at
#           the window's resolution: a label at half scale through FSR 1 and
#           TAA is the same pixels as at full scale with no anti-aliasing.
#   sprites a picture of sprites alone is drawn at the window's resolution and
#           takes no temporal pass: half scale through FSR 1 and TAA is the
#           same frame as full scale through SMAA.
#   mixed   pixel art among 3D surfaces is scaled by the nearest texel and never
#           jittered, blended, filtered or sharpened: at half scale -- FSR 1 and
#           TAA, TAA alone, FSR 1 and SMAA -- every pixel of a one-pixel
#           checkerboard is one of its two colours.
#
# Not a golden: each claim compares this machine's renders with each other or
# with the colours the scene gave, so it is a gate wherever there is a device.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCENES=<tests/screenshots/aa2d>
#         -DOUTPUT=<dir> -P run_aa_2d_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
set(FRAMES 24)

foreach(required HOST PROBE SCENES OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_aa_2d_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

# The picture's own changes held still, as the flicker gate holds them.
set(STILL --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows)

function(draw scene label)
    execute_process(
        COMMAND "${HOST}" "${SCENES}/${scene}" --headless "--frames=${FRAMES}" --exit --width=640 --height=360
                "--screenshot=${OUTPUT}/${label}.png" ${STILL} ${ARGN}
        RESULT_VARIABLE exited
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(exited EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(SKIPPED TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT exited EQUAL 0 OR NOT EXISTS "${OUTPUT}/${label}.png")
        message("${output}")
        message(FATAL_ERROR "anti-aliasing 2D gate: the host exited ${exited} and wrote no ${label}.png")
    endif()
endfunction()

function(probe what image)
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${image}.png" ${ARGN}
        RESULT_VARIABLE probed
        OUTPUT_VARIABLE report
        ERROR_VARIABLE report)
    message("${what}: ${report}")
    if(NOT probed EQUAL 0)
        message(FATAL_ERROR "anti-aliasing 2D gate: ${what} (see above); the frames are in ${OUTPUT}")
    endif()
endfunction()

set(SCALED --render-scale=0.5 --upscaling=fsr1 --anti-aliasing=taa)

# The UI: the label, two pixels inside its edges, at 40..340 by 40..120.
draw(ui ui-scaled ${SCALED})
if(SKIPPED)
    return()
endif()
draw(ui ui-full --render-scale=1 --upscaling=none --anti-aliasing=off)
probe("the label at half scale, FSR 1 and TAA is the label at full scale" ui-scaled --tolerance=0
      "--against=${OUTPUT}/ui-full.png" "0.065625,0.116667:0.528125,0.327778==")

# Sprites alone: the whole frame.
draw(sprites sprites-scaled ${SCALED})
draw(sprites sprites-full --render-scale=1 --upscaling=none --anti-aliasing=smaa)
probe("sprites alone at half scale, FSR 1 and TAA are drawn as at full scale" sprites-scaled --tolerance=0
      "--against=${OUTPUT}/sprites-full.png" "0,0:1,1==")

# Pixel art among 3D surfaces: the board, a render pixel inside its edges.
set(BOARD "0.278125,0.466667:0.321875,0.544444~200,80,40/10,200,250")
draw(mixed mixed-fsr-taa ${SCALED})
probe("pixel art through FSR 1 and TAA" mixed-fsr-taa --tolerance=1 "${BOARD}")
draw(mixed mixed-taa --render-scale=0.5 --upscaling=none --anti-aliasing=taa)
probe("pixel art through TAA, scaled by the resolve" mixed-taa --tolerance=1 "${BOARD}")
draw(mixed mixed-fsr-smaa --render-scale=0.5 --upscaling=fsr1 --anti-aliasing=smaa)
probe("pixel art through FSR 1 and SMAA" mixed-fsr-smaa --tolerance=1 "${BOARD}")
