# A morphed vertex lands where the file says (ADR 0196).
#
# Four pillars under a low sun, drawn twice: once from models with morph
# targets whose own weights make each two tall and leaning, and once from the
# same pillars made in that shape with no targets at all. The two pictures
# must be the same picture -- the lit faces, the depth the camera's prepass
# wrote, and the shadows on the floor -- which is what "the vertex lands where
# the file says" is, said of every pass that draws it and not only of one.
#
# What keeps that from being true of two wrong pictures:
#   - the run with targets must count its four bodies as `morph` draws, and
#     the run without must count none: the first went through the morph
#     pipelines and the second did not;
#   - neither run may warn -- a model that did not load is a picture of a
#     floor, twice;
#   - and a point above where the pillars stand at rest must be pillar and
#     not floor, so "the same" is not "both a unit tall".
#
# Asked of the graphics API the host picks, and of the one named by GPU when
# it is given: the shader finds a vertex's deltas by the vertex's number, and
# that number is one thing on Direct3D and another on Vulkan for a mesh that
# does not start at nought in its buffer (`MeshCache::attachMorphs`).
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DCOMPARE=<imgcmp> -DPROBE=<imgprobe>
#         -DSCRIPT=<project> -DOUTPUT=<dir> [-DGPU=<driver>]
#         -P run_morph_landing.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST COMPARE PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_morph_landing.cmake: -D${required}=... is required")
    endif()
endforeach()

set(device "")
if(DEFINED GPU AND NOT GPU STREQUAL "")
    set(device "--gpu=${GPU}")
endif()

# Two copies of the project, without whatever cache a run from the source
# tree left in it (D604): `morphed` as checked in, `made` with the models of
# `made/` in place of the ones with targets.
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(mode morphed made)
    file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/${mode}"
        PATTERN ".engine" EXCLUDE
        PATTERN "engine.log" EXCLUDE
        PATTERN "engine.previous.log" EXCLUDE)
endforeach()
# `COPY_FILE`, which always copies: `file(COPY)` leaves a file alone that has
# its source's timestamp, and the four models are written in the same second.
foreach(model plain skinned)
    file(COPY_FILE "${SCRIPT}/made/${model}.gltf" "${OUTPUT}/made/content/models/${model}.gltf")
endforeach()

foreach(mode morphed made)
    execute_process(
        COMMAND "${HOST}" "${OUTPUT}/${mode}" --headless --frames=100 --exit "--screenshot=${OUTPUT}/${mode}.png"
                --width=640 --height=360 --pace=60 --frame-report=1 ${device}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${mode}.png")
        message("${output}")
        message(FATAL_ERROR "morph landing: the host failed (${result}, ${mode})")
    endif()
    string(FIND "${output}" "[error]" errorAt)
    string(FIND "${output}" "[warn]" warnAt)
    if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
        message("${output}")
        message(FATAL_ERROR "morph landing: the run warned or failed (${mode})")
    endif()
    if(NOT output MATCHES "by what each was for: ([a-z_0-9, ]*)")
        message("${output}")
        message(FATAL_ERROR "morph landing: no frame report with its draws by kind (${mode})")
    endif()
    set(kinds_${mode} "${CMAKE_MATCH_1}")
    set(morph_${mode} 0)
    if(kinds_${mode} MATCHES "morph ([0-9]+)")
        set(morph_${mode} "${CMAKE_MATCH_1}")
    endif()
    message("morph landing: ${mode} --${kinds_${mode}}")
endforeach()

if(NOT morph_morphed EQUAL 4)
    message(FATAL_ERROR
        "morph landing: four bodies have targets above nought and ${morph_morphed} were drawn through the morph "
        "pipelines")
endif()
if(NOT morph_made EQUAL 0)
    message(FATAL_ERROR "morph landing: models with no targets counted ${morph_made} morph draws")
endif()

execute_process(
    COMMAND "${COMPARE}" "${OUTPUT}/morphed.png" "${OUTPUT}/made.png" --tolerance 3 --max-different-pixels 0
            --diff "${OUTPUT}/diff.png"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR
        "morph landing: pillars drawn with morph targets are not the pillars made in that shape (${OUTPUT}/diff.png)")
endif()

# The third pillar a unit and a half up, where there is only floor behind it
# if it is drawn at rest: clay, darker than the pale floor beside it.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/morphed.png" --tolerance=40 "0.534,0.417<0.47,0.417"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "morph landing: nothing stands above where the pillars are at rest")
endif()
