# A terrain layer reads only the channels its maps promise (terrain audit
# TA13).
#
# The project is copied for each run, one of its `variants/` put in as the ground's
# material each time: a metallic-roughness map whose R is empty -- glTF leaves R
# undefined unless the image is also the occlusion map -- and no map at all. The
# map's G and B say what the material's factors say, so the two pictures must
# be one picture. The terrain read R as occlusion, and as height: the first
# drew its ambient at four tenths.
#
# **And a normal map's up is up the image.** A third run wears a normal map
# tilted wholly towards +Y -- up the image, in glTF's convention -- on flat
# ground, photographed as the bend the shading gives the mesh's normal
# (`--debug-view=bend`, four times over about a mid grey). Up the image is
# down the ground plane's second axis, so the normal leans to -Z: no blue. The
# terrain bent it along +Z, lighting every bump from the wrong side along one
# axis.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DPROBE=<imgprobe>
#         -DSCRIPT=<project> -DOUTPUT=<dir> -P run_terrain_channels.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST IMGCMP PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_channels.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

foreach(variant mapped plain tilted)
    set(project "${OUTPUT}/${variant}")
    file(COPY "${SCRIPT}/" DESTINATION "${project}" PATTERN "variants" EXCLUDE)
    file(COPY_FILE "${SCRIPT}/variants/${variant}.material.json" "${project}/content/materials/ground.material.json")
    set(view "")
    if(variant STREQUAL "tilted")
        set(view "--debug-view=bend")
    endif()
    execute_process(
        COMMAND "${HOST}" "${project}" --headless --frames=30 --exit "--screenshot=${OUTPUT}/${variant}.png" ${view}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${variant}.png")
        message("${output}")
        message(FATAL_ERROR "terrain channels: the host failed rendering ${variant} (${result})")
    endif()
endforeach()

execute_process(
    COMMAND "${IMGCMP}" "${OUTPUT}/mapped.png" "${OUTPUT}/plain.png" --tolerance 2 --max-different-pixels 0
        "--diff" "${OUTPUT}/mapped.diff.png"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message("ENG_TERRAIN_CHANNELS_FAULT")
    message(FATAL_ERROR "terrain channels: a metallic-roughness map with nothing in R changed the ground")
endif()

# The flat ground in the picture's lower corners: red a mid grey (no bend
# across), green none (the normal leans off the ground's up) and blue none (it
# leans to -Z). The grain's own bend is a few levels either way.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/tilted.png" --tolerance=40 "0.15,0.92=119,0,0" "0.85,0.92=119,0,0"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message("ENG_TERRAIN_CHANNELS_FAULT")
    message(FATAL_ERROR "terrain channels: a normal map tilted up the image bent the ground the wrong way")
endif()
