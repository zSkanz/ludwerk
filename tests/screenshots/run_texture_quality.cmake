# A machine short of memory loads less of each texture, and only of the
# textures that can spare it (D609).
#
# One scene at two texture qualities: a wall that wears a material, a sprite,
# and a picture that a material and a sprite both name -- three pictures of
# 256 pixels, compiled as a project's content is.
#
# At `High` every texture is on the graphics card whole, and the frame report
# says nothing of texture quality. At `Low` the report says the world's
# textures are loaded without their two largest levels, and that exactly ONE
# is on the card smaller than its file: the wall's. The sprite's is drawn at
# its own size and is never reduced; and the picture both name is whole,
# whichever of the two asked first -- a tileset blurred because a material
# somewhere wore the same file would be a defect nobody could find.
#
# The quality is set as a game sets it, in the project file: what a machine's
# memory starts it at is held by `project_config_tests.cpp`.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSCRIPT=<project> -DOUTPUT=<dir> -P run_texture_quality.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_texture_quality.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(quality high low)
    # A copy each, without whatever cache a run from the source tree left
    # (D604), and with the quality said in its project file.
    file(COPY "${SCRIPT}/" DESTINATION "${OUTPUT}/${quality}"
        PATTERN ".engine" EXCLUDE
        PATTERN "engine.log" EXCLUDE
        PATTERN "engine.previous.log" EXCLUDE)
    file(APPEND "${OUTPUT}/${quality}/project.toml" "\n[graphics]\ntexture_quality = \"${quality}\"\n")

    execute_process(
        COMMAND "${HOST}" "${OUTPUT}/${quality}" --headless --frames=100 --exit --pace=60 --frame-report=1
                --width=320 --height=180 "--screenshot=${OUTPUT}/${quality}.png"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0)
        message("${output}")
        message(FATAL_ERROR "texture quality: the host failed (${result}, ${quality})")
    endif()
    string(FIND "${output}" "[error]" errorAt)
    string(FIND "${output}" "[warn]" warnAt)
    if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
        message("${output}")
        message(FATAL_ERROR "texture quality: the run warned or failed (${quality})")
    endif()
    # The pictures were compiled: a loose picture has no levels to leave out,
    # and a run that loaded them loose would prove nothing either way.
    if(NOT output MATCHES "Compiled [0-9]+ mesh\\(es\\) and 3 texture\\(s\\)")
        message("${output}")
        message(FATAL_ERROR "texture quality: the project's three pictures were not compiled (${quality})")
    endif()

    set(said_${quality} FALSE)
    set(levels_${quality} 0)
    set(reduced_${quality} 0)
    if(output MATCHES "Texture quality: [^\n]*without their ([0-9]+) largest[^\n]*; ([0-9]+) texture")
        set(said_${quality} TRUE)
        set(levels_${quality} "${CMAKE_MATCH_1}")
        set(reduced_${quality} "${CMAKE_MATCH_2}")
    endif()
    message("texture quality: ${quality} -- ${levels_${quality}} level(s) left out, ${reduced_${quality}} smaller")
endforeach()

if(said_high)
    message(FATAL_ERROR "texture quality: at High the report speaks of textures loaded smaller")
endif()
if(NOT said_low OR NOT levels_low EQUAL 2)
    message(FATAL_ERROR "texture quality: at Low the world's textures are not loaded without two levels")
endif()
if(NOT reduced_low EQUAL 1)
    message(FATAL_ERROR
        "texture quality: at Low ${reduced_low} textures are smaller than their file, and it should be one -- the "
        "wall's; the sprite's and the one a sprite shares are whole")
endif()
