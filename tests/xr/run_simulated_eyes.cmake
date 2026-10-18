# A frame drawn for a headset is two pictures, one for each eye, and they are
# not the same picture (ADR 0244, under the simulated headset of ADR 0248 §4).
#
# The host runs a scene headless with `--xr=simulated` and a screenshot: the
# window's picture and each eye's are written. Both eyes must be there, at the
# simulated headset's size, and differ -- the eyes are 63 mm apart and each
# sees further outward than toward the nose -- while each is a picture of the
# scene and not a cleared target.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST SCENE WORKDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_simulated_eyes.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(shot "${WORKDIR}/xr.png")
execute_process(
    COMMAND "${HOST}" "${SCENE}" --headless --frames=20 --exit --rhi=sdlgpu --xr=simulated "--screenshot=${shot}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(output MATCHES "rhi.err.device_create_failed|Could not create a graphics device")
    message("ENG_TEST_SKIP: no GPU device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the host failed (${result}):\n${output}")
endif()

foreach(eye left right)
    if(NOT EXISTS "${WORKDIR}/xr-${eye}.png")
        message(FATAL_ERROR "no picture for the ${eye} eye:\n${output}")
    endif()
    file(SIZE "${WORKDIR}/xr-${eye}.png" bytes)
    # A cleared 1024 by 1024 picture compresses to a few kilobytes; a scene
    # does not.
    if(bytes LESS 20000)
        message(FATAL_ERROR "the ${eye} eye's picture is ${bytes} bytes: nothing was drawn into it")
    endif()
endforeach()
# **The window is the mirror** (ADR 0244 §5): the left eye, cropped to the
# window's shape, and not a view of its own -- which is no longer drawn.
file(SIZE "${shot}" bytes)
if(bytes LESS 20000)
    message(FATAL_ERROR "the window's picture is ${bytes} bytes: the left eye was not mirrored into it")
endif()
file(SHA256 "${WORKDIR}/xr-left.png" left)
file(SHA256 "${WORKDIR}/xr-right.png" right)
if(left STREQUAL right)
    message(FATAL_ERROR "the two eyes drew the same picture")
endif()
message("two eyes drawn, they differ, and the window mirrors the left")
