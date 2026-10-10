# A morphed vertex lands where its weights say (ADR 0196).
#
# Four pillars under a low sun, drawn three times: from models with morph
# targets whose own weights make each two tall and leaning (`morphed`); from
# the same targets with every weight at nought in the file, shaped by the
# scene's script through `SetMorphWeight` and by a clip's weight channels
# (`driven`); and from the same pillars made in that shape with no targets at
# all (`made`). The three pictures must be the same picture -- the lit faces,
# the depth the camera's prepass wrote, and the shadows on the floor -- which
# is what "the vertex lands where its weights say" is, said of every pass that
# draws it, and of every way a weight is set.
#
# What keeps that from being true of three wrong pictures:
#   - the runs with targets must each count their four bodies as `morph`
#     draws, and the run without must count none: the first two went through
#     the morph pipelines and the third did not;
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

# Three copies of the project, without whatever cache a run from the source
# tree left in it (D604): `morphed` as checked in, and `driven` and `made`
# with the models of those folders in place of the checked-in ones.
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(mode morphed driven made)
    file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/${mode}"
        PATTERN ".engine" EXCLUDE
        PATTERN "engine.log" EXCLUDE
        PATTERN "engine.previous.log" EXCLUDE)
endforeach()
# `COPY_FILE`, which always copies: `file(COPY)` leaves a file alone that has
# its source's timestamp, and the models are all written in the same second.
foreach(mode driven made)
    foreach(model plain skinned)
        file(COPY_FILE "${SCRIPT}/${mode}/${model}.gltf" "${OUTPUT}/${mode}/content/models/${model}.gltf")
    endforeach()
endforeach()

foreach(mode morphed driven made)
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
    # **Asked of an API this machine does not have, the host says so and draws
    # through another** -- and then this is not the test that was asked for. A
    # hosted Windows runner has no Vulkan: `--gpu=vulkan` there drew through
    # Direct3D on the software device, the picture was right, and the run
    # failed for the warning that said it had fallen back. A skip, which the
    # gate on a machine that has the API does not allow.
    if(NOT device STREQUAL "" AND NOT output MATCHES ", through ${GPU}\\.")
        message("ENG_TEST_SKIP: no ${GPU} device on this machine; the host drew through another")
        return()
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

foreach(mode morphed driven)
    if(NOT morph_${mode} EQUAL 4)
        message(FATAL_ERROR
            "morph landing: four bodies have targets above nought and ${morph_${mode}} were drawn through the morph "
            "pipelines (${mode})")
    endif()
endforeach()
if(NOT morph_made EQUAL 0)
    message(FATAL_ERROR "morph landing: models with no targets counted ${morph_made} morph draws")
endif()

foreach(mode morphed driven)
    execute_process(
        COMMAND "${COMPARE}" "${OUTPUT}/${mode}.png" "${OUTPUT}/made.png" --tolerance 3 --max-different-pixels 0
                --diff "${OUTPUT}/diff-${mode}.png"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${output}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "morph landing: pillars drawn with morph targets are not the pillars made in that shape "
            "(${mode}: ${OUTPUT}/diff-${mode}.png)")
    endif()
endforeach()

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
