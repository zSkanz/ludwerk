# The same world point stays covered as the camera crosses cascade boundaries.
# The paired frame without a caster is lit, so an empty or black frame fails.
foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_shadow_reach_gate.cmake: -D${required}=... is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUTPUT}")
foreach(offset 0 10 20 -10)
    foreach(casts true false)
        set(picture "${OUTPUT}/camera-${offset}-caster-${casts}.png")
        file(REMOVE "${picture}")
        execute_process(
            COMMAND "${HOST}" "${SCRIPT}" --headless --frames=12 --exit --locale=en
                --width=800 --height=600 --quality=high --debug-view=shadow --no-contact-shadows
                "--scene-data={\"CameraX\":${offset},\"Cast\":${casts}}"
                "--screenshot=${picture}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(result EQUAL 4)
            message("ENG_TEST_SKIP: no graphics device on this machine")
            return()
        endif()
        if(NOT result EQUAL 0 OR NOT EXISTS "${picture}")
            message(FATAL_ERROR "shadow reach gate: host failed (${result}): ${output}${error}")
        endif()
        if(casts)
            set(probe "0.5,0.5=0,0,0")
        else()
            set(probe "0.5,0.5!=0,0,0")
        endif()
        execute_process(
            COMMAND "${PROBE}" "${picture}" --tolerance=12 "${probe}"
            RESULT_VARIABLE probed OUTPUT_VARIABLE details ERROR_VARIABLE problem)
        if(NOT probed EQUAL 0)
            message(FATAL_ERROR "shadow reach gate: camera ${offset}, caster ${casts}: ${details}${problem}")
        endif()
    endforeach()
endforeach()
message("shadow reach gate: the fixed receiver stays covered in every camera position; removing the caster lights it")
