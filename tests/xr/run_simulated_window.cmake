# **A headset and a window** (ADR 0244): the simulated headset drawing while
# the game has a real window, hidden, through the first headset frames and the
# change of the window's present mode they make -- started with VSync on, which
# makes it, and off, which does not. On a real headset the change once came
# after the frame had taken its picture from the window, and the game died at
# the present. A machine with no display, or no device, says so and is skipped.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST SCENE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_simulated_window.cmake needs -D${required}=")
    endif()
endforeach()

foreach(sync --vsync --no-vsync)
    execute_process(
        COMMAND "${HOST}" "${SCENE}" --xr=simulated --hidden-window ${sync} --frames=40 --exit
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(output MATCHES "rhi.err.device_create_failed|Could not create a graphics device|platform.err.window_failed|platform.err.init_failed|Could not create the game window|Could not start the platform layer")
        message("ENG_TEST_SKIP: no window or no GPU device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the host with a window and a headset failed with ${sync} (${result}):\n${output}")
    endif()
    if(NOT output MATCHES "Ran 40 frames")
        message(FATAL_ERROR "the host did not run its frames with ${sync}:\n${output}")
    endif()
    # **A window without the keyboard does not slow the headset**: the
    # hidden window never has it, and the headset in use paces the frames,
    # not the background rate.
    if(output MATCHES "is in the background")
        message(FATAL_ERROR "the headset was held to the window's background rate with ${sync}:
${output}")
    endif()
    # Started with VSync on, the window is asked for VSync off once the
    # headset draws: the change this test is for.
    if(sync STREQUAL "--vsync" AND NOT output MATCHES "VSync requested: on.*VSync requested: off")
        message(FATAL_ERROR "the window's present mode did not change when the headset began to draw:\n${output}")
    endif()
endforeach()
message("a window and a headset draw together, VSync on and off at the start")
