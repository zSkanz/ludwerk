# `examples/15-multiplayer` with a second player at the joining machine
# (ADR 0208), in two processes over the real transport.
#
# A server, and a joiner whose link is held 75 ms each way. The example's own
# test run (`--scene-data={"guest":true}`) seats a guest, holds both sticks to
# the right for two seconds and says how far each racer went ON THE JOINER:
# sixteen metres at eight a second, for a racer the joiner predicts. A racer
# it only follows is where the authority last said -- a round trip short, a
# metre and more -- so the one line is the whole check: both predicted, both
# the full distance.
#
#   cmake -DHOST=<engine-host> -DPROJECT=<examples/15-multiplayer> -DSTAGE=<dir>
#         -DPORT=<n> -P run_example_guest.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_example_guest.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE PATTERN "*.log" EXCLUDE)

execute_process(
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--serve=${PORT}" --frames=780 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=540 --net-delay=75
            "--scene-data={\"guest\":true}" "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

set(failures "")
string(REGEX MATCH "multiplayer: demo, [^\n]*" said "${output}")
if(said STREQUAL "")
    string(APPEND failures "  the example's test run said nothing\n")
else()
    message("${said}")
    string(REGEX MATCH "demo, ([0-9]+) racer\\(s\\) predicted here, the first went ([0-9.]+) m and the guest ([0-9.]+) m"
                       _ "${said}")
    set(predicted "${CMAKE_MATCH_1}")
    set(first "${CMAKE_MATCH_2}")
    set(guest "${CMAKE_MATCH_3}")
    if(NOT predicted EQUAL 2)
        string(APPEND failures "  ${predicted} racer(s) predicted on the joiner, where it has two players\n")
    endif()
    # Eight metres a second for two seconds. A tenth either way is a tick.
    foreach(who first guest)
        if(${who} LESS 15.8 OR ${who} GREATER 16.2)
            string(APPEND failures
                 "  the ${who} player's racer went ${${who}} m on the joiner in the two seconds its stick was held (16.0)\n")
        endif()
    endforeach()
endif()
string(FIND "${output}" "[warn]" warned)
string(FIND "${output}" "[error]" erred)
if(NOT warned EQUAL -1 OR NOT erred EQUAL -1)
    string(APPEND failures "  one of the two ends warned or erred\n")
endif()

if(NOT failures STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "the multiplayer example with a guest: what did not hold --\n${failures}")
endif()
message("the multiplayer example with a guest: both of the joiner's racers answered their sticks at once")
