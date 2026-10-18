# A player's record kept between runs of a server (ADR 0238 §2): a listen
# host loads its player's record, adds a level and lets it go; a second run
# with the same saves finds the level the first wrote.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST SCRIPT WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_record.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
foreach(run 1 2)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=60 --exit "--host=${PORT}" "--saves=${WORKDIR}/saves"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "run ${run} failed (${result}):\n${output}")
    endif()
    if(NOT output MATCHES "\\[data\\] level=${run} bag=2 same=true released=true refused=true")
        message(FATAL_ERROR "run ${run} did not find level ${run}:\n${output}")
    endif()
endforeach()
if(NOT EXISTS "${WORKDIR}/saves/data.db")
    message(FATAL_ERROR "the server's database is not in its saves folder")
endif()
message("the record is kept between runs of the server")
