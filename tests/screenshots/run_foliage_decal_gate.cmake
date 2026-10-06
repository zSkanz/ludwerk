# A decal paints the foliage that stands in it (ADR 0185), asked of two
# screenshots of one scene: two fields of tall upright cards filling the
# picture, one before a red mark is laid down over the middle of both and one
# after. The left field receives decals and the right has
# `FoliageLayer.ReceivesDecals` off.
#
# The mark points straight down, at every card edge-on. A decal fades where a
# surface turns away from it, so that a mark on a floor does not smear down a
# step's side -- and that fade left every card clean but for a line of pixels
# where one card's edge met the next.
#
# The project is copied with the card the foliage cards' test draws, whole.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DCARDS=<the foliagecards project> -DOUTPUT=<dir> -P run_foliage_decal_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT CARDS OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_foliage_decal_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
set(project "${OUTPUT}/project")
file(COPY "${SCRIPT}/" DESTINATION "${project}")
file(MAKE_DIRECTORY "${project}/content/models")
file(COPY_FILE "${CARDS}/content/models/card.gltf" "${project}/content/models/card.gltf")
file(COPY_FILE "${CARDS}/variants/solid.png" "${project}/content/models/card.png")

foreach(shot IN ITEMS "before|10" "after|40")
    string(REPLACE "|" ";" fields "${shot}")
    list(GET fields 0 name)
    list(GET fields 1 frames)
    execute_process(
        COMMAND "${HOST}" "${project}" --headless "--frames=${frames}" --exit "--screenshot=${OUTPUT}/${name}.png"
                --width=1000 --height=500 --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows
                --no-anti-aliasing
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${name}.png")
        message("${host_output}")
        message(FATAL_ERROR "foliage decal gate: the host exited ${host_result} and wrote no ${name} picture")
    endif()
endforeach()

# Twenty metres across 1000 pixels: x = (metres + 10) / 20. The mark is eight
# metres either side of the middle, 0.1 to 0.9; the receiving field is the
# left half and the refusing one the right. A card is some metres wide and
# stands where its root is, so the cards of each field reach over the line
# between them: nothing is asked of the middle fifth, which is both.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/after.png" --tolerance=12 "--against=${OUTPUT}/before.png"
        # The receiving field's cards inside the mark are darker than its cards
        # outside it: here, there and everywhere, not on one line of pixels.
        "0.13,0.22<0.04,0.22"
        "0.18,0.41<0.04,0.41"
        "0.24,0.57<0.04,0.57"
        "0.30,0.73<0.04,0.73"
        "0.35,0.12<0.04,0.12"
        "0.21,0.80<0.04,0.80"
        # The refusing field's are what they were before there was a mark:
        # every pixel of them, inside the mark and outside it.
        "0.66,0.05:0.98,0.95=="
        # And the receiving field's own, outside the mark.
        "0.01,0.05:0.08,0.95=="
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "foliage decal gate: a probe disagreed (see above); the frames are in ${OUTPUT}")
endif()
