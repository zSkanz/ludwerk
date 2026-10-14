# The thin wall (ADR 0203), asked of one screenshot: a room shut on every side
# beside a room open to the sky, ten centimetres of wall between them, each
# with an `IrradianceVolume` of its own. The shut room's only light is one
# bright panel; a dim panel beside it is the measure.
#
#   - The wall it shares with the bright room is DARKER than the dim panel. A
#     surface reads the volume a short step in front of it, so the dark face
#     of the wall is the shut room's however thin the wall is; with the flat
#     ambient in the volume's place, or with one point of the bright room's
#     light coming through, it is several times brighter than the measure.
#   - The ceiling beside the bright panel is BRIGHTER than the ceiling across
#     the room: the panel's light is bounced, which nothing but the volume
#     works out.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_thin_wall_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_thin_wall_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")
file(REMOVE "${OUTPUT}/shut.png")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=40 --exit "--screenshot=${OUTPUT}/shut.png"
            --width=1000 --height=500 --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows
            --no-anti-aliasing --global-illumination=high --reflections=low
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/shut.png")
    message("${host_output}")
    message(FATAL_ERROR "thin wall gate: the host exited ${host_result} and wrote no picture")
endif()

execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/shut.png" --tolerance=12
        # The shared wall, in three places, against the dim panel.
        "0.380,0.460<0.603,0.484"
        "0.300,0.400<0.603,0.484"
        "0.480,0.500<0.603,0.484"
        # The foot of that wall, where a wider blend between the two volumes
        # once let the bright room a hand's width onto this floor.
        "0.400,0.616<0.603,0.484"
        # The panel's light, bounced: the ceiling beside it against the
        # ceiling across the room.
        "0.760,0.300>0.350,0.120"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "thin wall gate: a probe disagreed (see above); the frame is in ${OUTPUT}")
endif()
