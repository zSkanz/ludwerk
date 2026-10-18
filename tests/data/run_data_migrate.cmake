# A record migrated (ADR 0238 §2): version 1 of a game writes it, version 2
# finds it older, migrates it once, and a second run of version 2 does not.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST V1 V2 WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_migrate.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
foreach(step "V1:wrote version 1" "V2:migrated=1 gold=30 old=nil" "V2:migrated=0 gold=30 old=nil")
    string(FIND "${step}" ":" colon)
    string(SUBSTRING "${step}" 0 ${colon} project)
    math(EXPR after "${colon} + 1")
    string(SUBSTRING "${step}" ${after} -1 expected)
    execute_process(
        COMMAND "${HOST}" "${${project}}" --headless --frames=60 --exit "--host=${PORT}" "--saves=${WORKDIR}/saves"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0 OR NOT output MATCHES "\\[migrate\\] ${expected}")
        message(FATAL_ERROR "${project} did not say '${expected}' (${result}):\n${output}")
    endif()
endforeach()
message("the record was migrated once, and kept at the new version")
