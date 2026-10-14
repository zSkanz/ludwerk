# The two-process sequence gate (ADR 0204, protocol 47).
#
# A server plays sequences; a joiner, which plays nothing, sees them. What
# travels is that a player is playing and from when -- nothing a tick, and
# nothing of the file, which both ends have -- and the gate holds what a
# joiner must have all the same:
#
#   - a sequence that was already going when it joined is met where it has got
#     to, not at nought, and stays beside the server's from there;
#   - every event of one played while it watches, once, in order, then
#     `Finished`; and the same number of ticks on both ends;
#   - the lamp the sequence keys on the authority is the lamp here: half way up
#     half way through, and all the way at the end;
#   - a skip is heard as a skip: the events it passed marked, `Finished`
#     saying so, and the lamp where the sequence would have left it;
#   - a stop finishes nothing and goes back to nought.
#
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/cutscene> -DSTAGE=<dir>
#         -DPORT=<n> -P run_cutscene.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_cutscene.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)

execute_process(
    COMMAND "${HOST}" --headless --pace=60 "--serve=${PORT}" --frames=3000 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=2400 "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

set(failures "")
function(line_of out what)
    string(REGEX MATCH "\\[cutscene\\] ${what}[^\n]*" found "${output}")
    set(${out} "${found}" PARENT_SCOPE)
    if(found STREQUAL "")
        set(failures "${failures}  the joiner said nothing of: ${what}\n" PARENT_SCOPE)
    endif()
endfunction()
function(holds line text)
    string(FIND "${line}" "${text}" at)
    if(at EQUAL -1)
        set(failures "${failures}  not `${text}` in: ${line}\n" PARENT_SCOPE)
    endif()
endfunction()
# A number a line gives under a name, in hundredths: `at=1.53` is 153.
function(hundredths_of out line name)
    string(REGEX MATCH "${name}=([0-9]+)\\.([0-9][0-9])" _ "${line}")
    if(CMAKE_MATCH_1 STREQUAL "")
        set(${out} "" PARENT_SCOPE)
    else()
        math(EXPR value "${CMAKE_MATCH_1} * 100 + 1${CMAKE_MATCH_2} - 100")
        set(${out} "${value}" PARENT_SCOPE)
    endif()
endfunction()
function(ticks_of out line)
    string(REGEX MATCH "ticks=([0-9]+)" _ "${line}")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Met already going: playing, a minute long, and not at nought -- the server
# began it before anybody joined.
line_of(arrived "arrived long ")
holds("${arrived}" "playing=true")
holds("${arrived}" "length=60.00")
hundredths_of(arrivedAt "${arrived}" "at")
if(arrivedAt STREQUAL "" OR arrivedAt LESS 5)
    string(APPEND failures "  a sequence already going was met at nought: ${arrived}\n")
endif()

# And beside the server's a second and a half on: within a tenth of a second,
# which is the trip and not a drift.
line_of(long "long server=")
hundredths_of(longThere "${long}" "server")
hundredths_of(longHere "${long}" "here")
if(longThere STREQUAL "" OR longHere STREQUAL "")
    string(APPEND failures "  no time for the long sequence on both ends: ${long}\n")
else()
    math(EXPR apart "${longHere} - ${longThere}")
    if(apart LESS -10 OR apart GREATER 10)
        string(APPEND failures "  the long sequence is at ${longThere} on the server and ${longHere} here\n")
    endif()
endif()
# Its first event was before the joiner: not said to it. And its last is a
# minute away.
string(FIND "${long}" "Begin" metBegin)
if(NOT metBegin EQUAL -1)
    string(APPEND failures "  an event from before it joined was fired for the joiner: ${long}\n")
endif()

# Played while it watched: every event once, in order, and the lamp.
line_of(act "act heard=")
holds("${act}" "heard=Begin,Middle=7,End,Finished ")
holds("${act}" "lamp=5.00")
holds("${act}" "at=3.00")
holds("${act}" "length=3.00")
line_of(serverAct "server act ")
holds("${serverAct}" "heard=Begin,Middle=7,End,Finished ")
holds("${serverAct}" "lamp=5.00")
ticks_of(seen "${act}")
ticks_of(played "${serverAct}")
if(seen STREQUAL "" OR played STREQUAL "")
    string(APPEND failures "  no count of ticks for the act\n")
else()
    if(played LESS 179 OR played GREATER 183)
        string(APPEND failures "  three seconds lasted ${played} ticks on the server\n")
    endif()
    math(EXPR apart "${seen} - ${played}")
    if(apart LESS -8 OR apart GREATER 8)
        string(APPEND failures "  the act lasted ${played} ticks on the server and ${seen} on the joiner\n")
    endif()
endif()
# Half way through, the lamp is part of the way up: it arrives as it is keyed.
string(REGEX MATCH "half=([0-9]+)\\.([0-9])" _ "${act}")
if(CMAKE_MATCH_1 STREQUAL "" OR CMAKE_MATCH_1 LESS 1 OR CMAKE_MATCH_1 GREATER 3)
    string(APPEND failures "  a second into the act the joiner's lamp was not between 1 and 4: ${act}\n")
endif()

# Skipped: the two events it passed are marked, and so is the end.
line_of(cut "cut heard=")
holds("${cut}" "heard=Begin,Middle=7,End!,Finished! ")
holds("${cut}" "lamp=5.00")
holds("${cut}" "at=3.00")
line_of(serverCut "server cut ")
holds("${serverCut}" "heard=Begin,Middle=7,End!,Finished!")
ticks_of(cutTicks "${cut}")
if(cutTicks STREQUAL "" OR cutTicks LESS 50 OR cutTicks GREATER 75)
    string(APPEND failures "  a sequence skipped a second in lasted ${cutTicks} ticks on the joiner\n")
endif()

# Stopped: one event more -- the replay's first -- and nothing finished.
line_of(stopped "stopped heard=")
holds("${stopped}" "heard=Begin,Middle=7,End,Finished,Begin ")
holds("${stopped}" "at=0.00")
holds("${stopped}" "playing=false")

line_of(done "done")
string(FIND "${output}" "[warn]" warned)
string(FIND "${output}" "[error]" erred)
if(NOT warned EQUAL -1 OR NOT erred EQUAL -1)
    string(APPEND failures "  one of the two ends warned or erred\n")
endif()

message("${arrived}
${long}
${act}
${serverAct}
${cut}
${serverCut}
${stopped}")

if(NOT failures STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "cutscene: a joiner did not see the sequences the server played as it should:\n${failures}")
endif()
message("cutscene: a joiner saw what the server played -- met part way, heard in order, skipped and stopped")
