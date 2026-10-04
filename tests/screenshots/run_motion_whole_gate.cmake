# A thing that moves has moved over the whole of its face (D548).
#
# A slab crossing in front of a wall, the camera sliding past both, drawn with
# `--debug-view=motion`: each pixel is the colour of how far it moved since
# the last frame -- red along x, mid grey for none and the whole channel for
# sixteen pixels. The slab crosses seven pixels a frame and the wall under one.
#
# The slab's motion is drawn over the camera's own, tested against the depth
# the picture's depth pass wrote. That pass multiplies a vertex by the model
# and then by the camera; the motion pass multiplied it by their product, made
# on the CPU -- another rounding, so on half the face the slab lost to its own
# depth and those pixels kept the wall's motion. The temporal pass then looked
# for them in the last frame where the wall was, and frame generation carried
# them half as far as the pixels beside them.
#
# `imgprobe`: every pixel of the middle of the slab is the slab's motion, and
# the wall beside it is not.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<motionwhole>
#         -DOUTPUT=<directory> -P run_motion_whole_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_motion_whole_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")
file(REMOVE "${OUTPUT}/motion.png")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=20 --exit "--screenshot=${OUTPUT}/motion.png" --width=640
            --height=360 --debug-view=motion --no-auto-exposure --no-bloom
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/motion.png")
    message("${host_output}")
    message(FATAL_ERROR "motion whole gate: the host exited ${host_result} and wrote no picture")
endif()

# The slab is from 0.42 to 0.86 of the picture across and 0.28 to 0.74 down.
# 178 is seven pixels a frame to the right; the wall's 127 is fifty-one under
# it, four times the tolerance.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/motion.png" --tolerance=12 "0.45,0.32:0.84,0.70~178,127,255"
            "0.30,0.50!=178,127,255" "0.95,0.50!=178,127,255"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "motion whole gate: part of a moving face did not move with it, or the wall moved as the "
                        "slab does (see above); the frame is ${OUTPUT}/motion.png")
endif()
