# `BasePart.ReceivesDecals`, asked of two screenshots of one scene: one before
# a decal is laid over a floor and two boxes on it, one after. The box that
# receives is darker than a box the decal does not reach, and so is the floor
# between them; the box with `ReceivesDecals` off is the same pixels in both
# pictures -- the decal painted round it and under it, and not on it.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_decal_receives_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_decal_receives_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")
foreach(shot IN ITEMS "before|10" "after|40")
    string(REPLACE "|" ";" fields "${shot}")
    list(GET fields 0 name)
    list(GET fields 1 frames)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit "--screenshot=${OUTPUT}/${name}.png"
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
        message(FATAL_ERROR "decal receives gate: the host exited ${host_result} and wrote no ${name} picture")
    endif()
endforeach()

# Twenty metres across 1000 pixels: x = (metres + 10) / 20, on the middle row.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/after.png" --tolerance=12 "--against=${OUTPUT}/before.png"
        # The floor inside the mark is darker than the floor outside it.
        "0.5,0.5<0.95,0.5"
        # The box that receives is darker than the box the mark does not reach.
        "0.41,0.5<0.85,0.5"
        # The box that does not is what it was before there was a mark: every
        # pixel of its top.
        "0.56,0.44:0.62,0.56=="
        # And nothing else changed where the mark is not.
        "0.82,0.44:0.88,0.56=="
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "decal receives gate: a probe disagreed (see above); the frames are in ${OUTPUT}")
endif()
