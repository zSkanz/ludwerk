# The CPU's rules are the shader's (terrain audit T3; ADR 0113 section 2
# promised a test, and there was none).
#
# The project is copied twice and run as the layer each pixel is drawn as
# (`--debug-view=material`): once drawn by its rules, and once after the scene
# has written what the CPU says they draw into the voxels (`ApplyRules`) and
# turned them off. What a game is told the ground is -- a raycast's material,
# the foliage's -- is that CPU answer, so the two pictures must agree.
#
# **But for the edges**: a voxel's slope is its occupancy's gradient at its
# centre, and a pixel's is the mesh's normal where it falls, so a rule's edge
# moves by up to a voxel between the two, as ragged as its noise. Measured, 2
# per cent of the picture differs, all of it along edges; a rule the CPU drew
# on the wrong hill, or not at all, is several times that.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_terrain_rules.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# Two and a half per cent of 960 by 540: the edges measure two, and a slope
# the CPU read a fifth too shallow measured three.
set(MAX_DIFFERENT 13000)

foreach(required HOST IMGCMP SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_terrain_rules.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

foreach(variant drawn applied)
    set(project "${OUTPUT}/${variant}")
    file(COPY "${SCRIPT}/" DESTINATION "${project}")
    if(variant STREQUAL "applied")
        file(WRITE "${project}/src/shared/applied.luau" "--!strict\nreturn true\n")
    endif()
    execute_process(
        COMMAND "${HOST}" "${project}" --headless --frames=30 --exit "--screenshot=${OUTPUT}/${variant}.png"
                --debug-view=material
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${variant}.png")
        message("${output}")
        message(FATAL_ERROR "terrain rules: the host failed rendering ${variant} (${result})")
    endif()
endforeach()

execute_process(
    COMMAND "${IMGCMP}" "${OUTPUT}/drawn.png" "${OUTPUT}/applied.png" --tolerance 2
            --max-different-pixels ${MAX_DIFFERENT} --diff "${OUTPUT}/drawn.diff.png"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message("ENG_TERRAIN_RULES_FAULT")
    message(FATAL_ERROR "terrain rules: what the CPU says the rules draw is not what the shader draws")
endif()
