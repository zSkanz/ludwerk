# A room that is ONE mesh (ADR 0203), asked of one screenshot from inside it:
# walls, ceiling, a tiled floor, a window and a door in a single file, as a
# building from a modelling program is.
#
# To the physics world the mesh is one hull with every opening shut, and a
# volume worked out against that has every point inside a solid: the room is
# black. Worked out against the mesh's own triangles, the sun and the sky come
# in through the openings it has. A dim panel on the east wall is the measure:
#
#   - the wall beside it is BRIGHTER than the panel, and
#   - so is the ceiling,
#
# neither of which is so when the mesh is asked by its hull
# (`--skip=bake_triangles` shows it).
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_mesh_room_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_mesh_room_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")
file(REMOVE "${OUTPUT}/inside.png")
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=40 --exit "--screenshot=${OUTPUT}/inside.png"
            --width=1000 --height=500 --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows
            --no-anti-aliasing --global-illumination=high --reflections=low
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/inside.png")
    message("${host_output}")
    message(FATAL_ERROR "mesh room gate: the host exited ${host_result} and wrote no picture")
endif()
# A mesh that is asked by its triangles is its own shape, and nothing warns.
if(host_output MATCHES "app.warn.irradiance_volume_meshes|as its collision shape")
    message("${host_output}")
    message(FATAL_ERROR "mesh room gate: the volume warned that a mesh was asked by its collision shape")
endif()

execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/inside.png" --tolerance=20
        "0.520,0.500>0.437,0.486"
        "0.600,0.120>0.437,0.486"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "mesh room gate: a probe disagreed (see above); the frame is in ${OUTPUT}")
endif()
