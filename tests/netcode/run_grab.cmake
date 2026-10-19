# A part held in a headset, over a real connection (`@engine/xrgrab`).
#
# A real server and two real joiners, each joiner 75 ms each way from it
# (`--net-delay`): 150 ms there and back. The first joiner holds block A
# and lifts it 30 cm: it is theirs (`SetNetworkOwner`) and the server sees
# it rise, and let go it is the authority's again after the settle. The
# second reaches for A while it is held and is refused; reaches for B from
# three metres off and is refused; reaches for B from where it is and holds
# it.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/grab> -DSTAGE=<dir>
#         -DPORT=<port> -P run_grab.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_grab.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)

# The server and the first joiner through `apart.cmake`, each into a file of
# its own; the second's output reaches this script.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -DOUT=server.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- "${HOST}" --headless
            --pace=60 "--serve=${PORT}" --frames=2400 "${game}"
    COMMAND "${CMAKE_COMMAND}" -DOUT=first.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- "${HOST}" --headless
            --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --net-delay=75 --frames=2000 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --net-delay=75 --frames=2000
            "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
set(all "${output}")
foreach(name server first)
    if(EXISTS "${game}/${name}.out")
        file(READ "${game}/${name}.out" text)
        string(APPEND all "\n${text}")
    endif()
endforeach()

set(failures "")
if(NOT all MATCHES "\\[grab\\] first held A=true rose 0\\.(2[5-9]|3[0-5]) m")
    string(APPEND failures "  the first joiner did not hold A and lift it 30 cm\n")
endif()
if(NOT all MATCHES "\\[grab\\] second A refused=true, B from afar refused=true, B near held=true")
    string(APPEND failures "  the second joiner was not refused A and B from afar, or did not hold B\n")
endif()
if(NOT all MATCHES "\\[grab\\] server A held by [23] rose 0\\.(2[5-9]|3[0-5]) m, then the authority's")
    string(APPEND failures "  the server did not see A held, lifted and given back\n")
endif()
if(NOT failures STREQUAL "")
    message("${all}")
    message(FATAL_ERROR "grab: a part held over the network --\n${failures}")
endif()
message("grab: held, lifted and given back; refused while held and from afar; held from near")
