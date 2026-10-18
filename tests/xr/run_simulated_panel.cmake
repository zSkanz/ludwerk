# A `ScreenGui` in a headset is a panel (ADR 0246 §1): run under the simulated
# headset with a GPU, laid out against the panel's canvas -- the button's size
# is half and three tenths of 1777.8 by 1000 -- and drawn into each eye; the
# right hand points at the button and pulls the trigger, and it is pressed
# once; then a fingertip pushed through it and out again presses it once
# more (§2).
cmake_minimum_required(VERSION 3.24)

foreach(required HOST SCENE WORKDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_simulated_panel.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(shot "${WORKDIR}/panel.png")
execute_process(
    COMMAND "${HOST}" "${SCENE}" --headless --frames=50 --exit --rhi=sdlgpu --xr=simulated "--screenshot=${shot}"
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
if(NOT output MATCHES "\\[panel\\] size=889x300")
    message(FATAL_ERROR "the screen was not laid out on the panel's canvas:\n${output}")
endif()
if(NOT output MATCHES "\\[panel\\] ray z=-0.30 along=-0.94")
    message(FATAL_ERROR "the right hand does not point where it was put:\n${output}")
endif()
if(NOT output MATCHES "\\[panel\\] poked=2")
    message(FATAL_ERROR "a fingertip pushed through the button and out again did not activate it once more:\n${output}")
endif()
if(NOT output MATCHES "\\[panel\\] activated=1")
    message(FATAL_ERROR "pointing at the button and pulling the trigger did not activate it once:\n${output}")
endif()
foreach(eye left right)
    if(NOT EXISTS "${WORKDIR}/panel-${eye}.png")
        message(FATAL_ERROR "no picture for the ${eye} eye:\n${output}")
    endif()
endforeach()
message("the screen is a panel, laid out on its canvas, and a hand pointing at its button, or a fingertip poking it, presses it")
