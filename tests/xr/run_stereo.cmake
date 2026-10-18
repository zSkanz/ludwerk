# A pair of eyes fuses (ADR 0244): a point of the world lands on the same row
# of both eyes' pictures, and the right eye sees it further left.
#
# The host draws three markers headless with `--xr=simulated`, whose eyes
# see further outward than inward and further down than up, as a real
# headset's do, from a head turned and tilted; `imgstereo` finds each marker
# in both pictures.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST STEREO SCENE WORKDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_stereo.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(shot "${WORKDIR}/stereo.png")
execute_process(
    COMMAND "${HOST}" "${SCENE}" --headless --frames=30 --exit --rhi=sdlgpu --xr=simulated "--screenshot=${shot}"
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
execute_process(
    COMMAND "${STEREO}" "${WORKDIR}/stereo-left.png" "${WORKDIR}/stereo-right.png" --rows=1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "the two eyes do not fuse")
endif()
