# Anchors kept between sessions (ADR 0247 §3): one run of the simulated
# headset pins and saves an anchor, a second run loads it, and it is the same
# anchor at the same place.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST SAVE LOAD WORKDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_simulated_anchors.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(store "${WORKDIR}/anchors.txt")
foreach(step SAVE LOAD)
    execute_process(
        COMMAND "${HOST}" "${${step}}" --headless --frames=30 --exit --rhi=null --xr=simulated
                "--xr-simulated-store=${store}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the ${step} run failed (${result}):\n${output}")
    endif()
    set(${step}_OUTPUT "${output}")
endforeach()
if(NOT SAVE_OUTPUT MATCHES "\\[anchor\\] saved=true id=([0-9a-f]+) tracked=true at=0\\.50,1\\.00,-1\\.00 look=-1\\.00,0\\.00")
    message(FATAL_ERROR "the first session did not pin and save the anchor:\n${SAVE_OUTPUT}")
endif()
set(id "${CMAKE_MATCH_1}")
if(NOT LOAD_OUTPUT MATCHES "\\[anchor\\] loaded=1 id=${id} tracked=true at=0\\.50,1\\.00,-1\\.00 look=-1\\.00,0\\.00 same=true")
    message(FATAL_ERROR "the second session did not find anchor ${id} where it was:\n${LOAD_OUTPUT}")
endif()
message("the anchor is where it was, in the next session")
