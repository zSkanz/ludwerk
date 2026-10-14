# A vehicle, looked at (ADR 0213).
#
# `examples/44-vehicle` driven with no hand on the keys -- full throttle, five
# seconds -- and a picture from the camera that follows it. What is held:
#
#   - it drove: the example says how far, and it is ten metres or more;
#   - the car is drawn where the camera looks, and its rear tyres under it:
#     each tyre darker than the road beside it, the body not the road's
#     colour. Asked as which point is darker, not as colours, so a software
#     device and a card draw the same claim;
#   - nothing warned.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DPROJECT=<examples/44-vehicle>
#         -DOUTPUT=<dir> -P run_vehicle_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)
foreach(required HOST PROBE PROJECT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_vehicle_gate.cmake: -D${required}=... is required")
    endif()
endforeach()
file(MAKE_DIRECTORY "${OUTPUT}")
set(picture "${OUTPUT}/driving.png")
file(REMOVE "${picture}")

execute_process(
    COMMAND "${HOST}" "${PROJECT}" --headless --frames=300 --exit --pace=60 --width=640 --height=360 --locale=en
            "--screenshot=${picture}" "--scene-data={\"throttle\":1}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "vehicle gate: the host failed (${result})")
endif()
string(FIND "${output}" "[error]" errorAt)
string(FIND "${output}" "[warn]" warnAt)
if(NOT errorAt EQUAL -1 OR NOT warnAt EQUAL -1)
    message("${output}")
    message(FATAL_ERROR "vehicle gate: the run warned or failed")
endif()
string(REGEX MATCH "drove ([0-9]+) m" _ "${output}")
if(CMAKE_MATCH_1 STREQUAL "" OR CMAKE_MATCH_1 LESS 10)
    message("${output}")
    message(FATAL_ERROR "vehicle gate: the car drove '${CMAKE_MATCH_1}' m in five seconds of full throttle")
endif()
message("vehicle gate: drove ${CMAKE_MATCH_1} m")
if(NOT EXISTS "${picture}")
    message(FATAL_ERROR "vehicle gate: no picture was written")
endif()

# The rear tyres, darker than the road below the car; the body, not the road.
execute_process(
    COMMAND "${PROBE}" "${picture}" --tolerance=60
            "0.453,0.661<0.5,0.9" "0.542,0.672<0.5,0.9" "0.5,0.6!=128,142,173"
    RESULT_VARIABLE probed
    OUTPUT_VARIABLE said
    ERROR_VARIABLE said)
message("${said}")
if(NOT probed EQUAL 0)
    message(FATAL_ERROR "vehicle gate: the car is not drawn as it should be; the picture is ${picture}")
endif()
