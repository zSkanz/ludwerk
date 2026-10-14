# What a wall broken into fifty pieces costs on the wire (ADR 0215).
#
# A real server and a real joiner over the real transport, 50 ms of round
# trip. The server breaks an eight-metre wall into fifty pieces on the
# joiner's word and strikes it; the joiner counts the bytes it receives a
# second while nothing happens, while the pieces fall, and once they lie
# still. Held:
#
#   - every piece arrives: fifty parts, sent as parts;
#   - cut and standing, before anything touches it, it costs nothing more
#     than the world did: the pieces held by their joints are asleep;
#   - falling, the wall costs what fifty moving parts cost, and no more than
#     the ceiling below -- the number the ADR states, measured;
#   - settled, the cost falls back: pieces at rest are not sent every tick.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/fracture> -DSTAGE=<dir>
#         -DPORT=<port> -P run_fracture.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_fracture.cmake: -D${required}=... is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${STAGE}")
file(COPY "${PROJECT}/" DESTINATION "${STAGE}" PATTERN ".engine" EXCLUDE)

execute_process(
    COMMAND "${HOST}" --headless --pace=60 "--serve=${PORT}" --frames=1200 "${STAGE}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=1080 "--net-delay=25"
            "${STAGE}"
    WORKING_DIRECTORY "${STAGE}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

string(REGEX MATCH "\\[fracture\\] [^\n]*" line "${output}")
if(line STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "fracture: the joiner printed no measurement")
endif()
message("${line}")
foreach(key made arrived quiet standing falling settled)
    string(REGEX MATCH " ${key}=([0-9]+)" _ " ${line}")
    set(${key} ${CMAKE_MATCH_1})
endforeach()

set(failures "")
if(NOT made EQUAL 50 OR NOT arrived EQUAL 50)
    string(APPEND failures "  fifty pieces were to be made and arrive: made ${made}, arrived ${arrived}\n")
endif()
# Measured where this was written (ADR 0215): cut and standing, nothing
# beyond the quiet line -- the pieces sleep; struck, 106 KB a second in the
# three seconds the pieces fall, about 70 bytes a moving piece a snapshot, and
# 39 KB in the three after. The ceiling is half again the falling figure,
# room for a runner's timing.
math(EXPR quietCeiling "${quiet} + 2000")
if(standing GREATER quietCeiling)
    string(APPEND failures "  cut and standing, the wall cost ${standing} bytes a second against ${quiet} quiet\n")
endif()
if(falling GREATER 160000)
    string(APPEND failures "  falling, the wall cost ${falling} bytes a second, over 160000\n")
endif()
if(NOT settled LESS falling)
    string(APPEND failures "  settled, the wall cost ${settled} bytes a second, no less than falling (${falling})\n")
endif()
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "fracture on the wire:\n${failures}")
endif()
