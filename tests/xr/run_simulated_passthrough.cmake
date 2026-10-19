# The room seen through (ADR 0247 §1): under the simulated headset, with a
# GPU, the left eye's picture is clear where the game drew nothing -- the sky
# over the horizon -- and opaque where it drew the floor below it; opaque where
# a sign that writes no depth is drawn over the sky, and clear where a part
# that shows the room stands (ADR 0250).
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROBE SCENE WORKDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_simulated_passthrough.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(shot "${WORKDIR}/room.png")
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
if(NOT output MATCHES "\\[passthrough\\] supported=true on=true")
    message(FATAL_ERROR "the simulated headset did not take the passthrough:\n${output}")
endif()
# Over the horizon, clear whatever its colour; under it, the floor, solid;
# the sign, solid; the window onto the room, clear.
execute_process(
    COMMAND "${PROBE}" "${WORKDIR}/room-left.png" --tolerance=2 "0.5,0.1=-1,-1,-1,0" "0.5,0.9=-1,-1,-1,255"
            "0.55,0.38=-1,-1,-1,255" "0.5,0.62=-1,-1,-1,0"
    RESULT_VARIABLE probed
    OUTPUT_VARIABLE probeOutput
    ERROR_VARIABLE probeOutput)
if(NOT probed EQUAL 0)
    message(FATAL_ERROR "the eye's alpha is not the room's:\n${probeOutput}")
endif()
message("the room shows where nothing is drawn")
