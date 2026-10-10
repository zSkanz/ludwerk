# A character the server animates is animated on a joiner, with no code on
# the joiner doing it (ADR 0197, protocol 44).
#
# A real server and a real joiner, two processes over the real transport. The
# server spawns ONE character from a stamp -- with its `AnimationPlayer` and
# the graph it names, a cape, a control, a foot placement, a light, a sound
# and a trail -- and, once somebody has joined, walks it, runs it, makes it
# jump and sets the one attribute that is its attack. It plays no animation.
#
# The joiner (`tests/netcode/animated/src/client/Watch.luau`) makes nothing and
# plays nothing: it waits for the character, finds the player that arrived
# with it, and says what that player's graph did on ITS machine. This holds
# every one:
#
#   - the player arrived, with the graph the stamp names;
#   - standing, walking and running, the graph took the character to be going
#     about nought, three and eight metres a second -- read from where the
#     character was, which is all a joiner has of it;
#   - the jump was a Jump, a Fall and a landing; the attack was a Slash that
#     began when the attribute changed and ended; the clips' events fired;
#   - what its graph did, in order, is what the server's own graph did;
#   - and the character's cape, control, feet, light, sound and trail are
#     there with what the stamp authored.
#
# Before protocol 44 the second line of the joiner's script never returned.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/animated> -DSTAGE=<dir>
#         -DPORT=<port> -P run_animated.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_animated.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE PATTERN "tools" EXCLUDE)

# Both at once, as the other two-process gates run theirs. The joiner draws
# nothing (`--rhi=null`): a graph is simulation and steps where nothing is
# drawn. The frames are the most a slow machine is given; the joiner ends the
# run when it has said what it saw, and the server ends when the joiner leaves.
execute_process(
    COMMAND "${HOST}" --headless --pace=60 "--serve=${PORT}" --frames=3600 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=3000 "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

set(failures "")
function(line_of out what)
    string(REGEX MATCH "\\[animated\\] ${what}[^\n]*" found "${output}")
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

line_of(player "player ")
holds("${player}" "graph=asset://anim/walker.animgraph.json")
holds("${player}" "retargeting=Automatic")

# The speeds, each within what a position read a tick apart and settled over
# a few can be off by: the walk is three and the run is eight.
line_of(speed "speed ")
foreach(entry "stand|0|0.5" "walk|2.4|3.6" "run|6.8|9.2")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 phase)
    list(GET parts 1 least)
    list(GET parts 2 most)
    string(REGEX MATCH " ${phase}=([0-9.]+)" _ "${speed}")
    if(CMAKE_MATCH_1 STREQUAL "")
        string(APPEND failures "  no speed read while it was to ${phase}: ${speed}\n")
    elseif(CMAKE_MATCH_1 LESS least OR CMAKE_MATCH_1 GREATER most)
        string(APPEND failures "  to ${phase}, the graph read ${CMAKE_MATCH_1} m/s (${least} to ${most}): ${speed}\n")
    endif()
endforeach()

line_of(saw "saw ")
foreach(thing jump fall land slash back step hit)
    holds("${saw}" "${thing}=true")
endforeach()

line_of(carried "carried ")
holds("${carried}" "cape=Cape_M0,0.5")
holds("${carried}" "reach=Arm,Grip,0.75,0.25")
holds("${carried}" "feet=Cape_M1,Body,0.375")
holds("${carried}" "lamp=12")
holds("${carried}" "hum=true,0.25")
holds("${carried}" "streak=1.5")

# The piece the character wears arrived wearing it: its `PoseFrom` names the
# joiner's own copy of the body (ADR 0201, protocol 45).
line_of(worn "worn ")
holds("${worn}" "piece=MeshPart")
holds("${worn}" "from=Body")
holds("${worn}" "same=true")

line_of(done "done")

# **The same states from the same inputs**: what the joiner's graph did, in
# order, is what the server's own did -- which the server kept in an
# attribute for the joiner to say beside its own.
line_of(joiner "joiner did ")
line_of(server "server did ")
string(REPLACE "[animated] joiner did " "" joinerDid "${joiner}")
string(REPLACE "[animated] server did " "" serverDid "${server}")
if(NOT joinerDid STREQUAL serverDid)
    string(APPEND failures "  the two graphs did not do the same:
    ${server}
    ${joiner}
")
endif()
message("${player}
${speed}
${saw}
${carried}
${server}
${joiner}")

if(NOT failures STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "animated: a joiner did not see the character the server animates:\n${failures}")
endif()
message("animated: a joiner saw the walk, the run, the jump and the attack, and made none of them")
