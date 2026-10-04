# Particles simulated on the GPU (ADR 0160), on a device with compute shaders:
#
#   1. They collide. Three emitters rain sparks that stick where they land --
#      on the terrain's height map, on the picture's depth, and on nothing --
#      and the ground under the first two is brighter than under the third.
#   2. They are cheap. A hundred thousand sparks alive at once, bouncing on
#      the ground, in a window with the display's sync off: the median frame
#      under a sixtieth of a second.
#
# Comparisons of this machine's own frame, and its own time: a gate wherever
# there is a device, skipped where there is none or it has no compute. The
# time is held only where a GPU draws: a software rasteriser -- the container's
# lavapipe, a runner's WARP -- simulates and draws on the CPU, and what it
# takes says nothing of the design.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCENE=<particlesgpu>
#         -DMANY=<particlesgpumany> -DOUTPUT=<png> -P run_particles_gpu_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
# Sixty frames a second.
set(BUDGET_MS 16.7)

foreach(required HOST PROBE SCENE MANY OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_particles_gpu_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE "${OUTPUT}")
execute_process(
    COMMAND "${HOST}" "${SCENE}" --headless --frames=420 --exit "--screenshot=${OUTPUT}" --width=1000 --height=500
            --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows --anti-aliasing=off
    RESULT_VARIABLE host_result
    OUTPUT_VARIABLE host_output
    ERROR_VARIABLE host_output)
if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    message("${host_output}")
    message(FATAL_ERROR "particles on the GPU: the host exited ${host_result} and wrote no screenshot")
endif()
# A device with no compute shaders simulates them on the CPU, and says so:
# nothing here to hold it to.
if(host_output MATCHES "no compute shaders")
    message("ENG_TEST_SKIP: this device has no compute shaders")
    return()
endif()

# Where each emitter's sparks land: x 0.393, 0.499 and 0.607 on the picture's
# middle row. The third is read just under it -- the bare ground, where
# nothing lies -- so a spark still falling past is not taken for one lying.
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}" --tolerance=60
        # The terrain's height map: sparks lying on the ground.
        "0.393,0.5>0.607,0.52"
        # The picture's depth: the same.
        "0.499,0.5>0.607,0.52"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR "particles on the GPU: a probe disagreed (see above); the frame is ${OUTPUT}")
endif()

# A hundred thousand, timed in a window: what a player's machine draws.
execute_process(
    # No cap, and none for a window nobody is looking at: a test's window is
    # opened behind whatever started it, and ten frames a second is what an
    # unfocused window is held to.
    COMMAND "${HOST}" "${MANY}" --frames=600 --exit --no-vsync --max-frame-rate=0 --background-frame-rate=0
            --frame-stats --width=1280 --height=720
    RESULT_VARIABLE timed_result
    OUTPUT_VARIABLE timed_output
    ERROR_VARIABLE timed_output)
if(NOT timed_result EQUAL 0)
    message("${timed_output}")
    message(FATAL_ERROR "particles on the GPU: the timed run exited ${timed_result}")
endif()
# The adapter, as the host says it where it says how the window presents.
if(timed_output MATCHES "llvmpipe|lavapipe|SwiftShader|softpipe|Basic Render Driver|WARP")
    message("a hundred thousand sparks: drawn by a software rasteriser here; the time is not held")
    return()
endif()
string(REGEX MATCH "median ([0-9.]+) ms" found "${timed_output}")
if(found STREQUAL "")
    message("${timed_output}")
    message(FATAL_ERROR "particles on the GPU: the timed run printed no frame statistics")
endif()
set(median "${CMAKE_MATCH_1}")
message("a hundred thousand sparks: median frame ${median} ms (at most ${BUDGET_MS})")
if(median GREATER BUDGET_MS)
    message(FATAL_ERROR "particles on the GPU: a hundred thousand sparks took ${median} ms a frame, over ${BUDGET_MS}")
endif()
