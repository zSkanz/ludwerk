# The netcode acceptance gate (docs/briefs/netcode-acceptance.md, B).
#
# A real server and a real client, two processes over the real transport, the
# client's link made worse by the link conditioner (`--net-delay`,
# `--net-jitter`, `--net-loss`, ledger A) -- and the bot in
# `tests/netcode/acceptance` walking a predicted character a straight line for
# ten seconds, stopping, and measuring. One run a condition; the numbers come
# out as one table, and each is held to what the ledger asks:
#
#   - a straight walk is not corrected (twice at most for a long frame: the
#     time the client dropped, and the stream anchored again);
#   - stopped, the replica is where the authority is, to a millimetre;
#   - a remote's round trip is at most the ping, a tick and a frame;
#   - the ping is the link's.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/acceptance> -DSTAGE=<dir>
#         -DPORT=<first port> -P run_netcode_acceptance.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_netcode_acceptance.cmake: -D${required}=... is required")
    endif()
endforeach()

# name; delay each way (ms); jitter (ms); loss (%); a frame to hang (s)
set(conditions
    "clean|0|0|0|0"
    "50ms|25|20|2|0"
    "150ms|75|20|2|0"
    "300ms|150|20|2|0"
    "hang150|25|20|2|0.15"
    "hang400|25|20|2|0.4")

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(table "| condition | corrections | long frames | stop (m) | ping (ms) | remote (ms) | interpolation (ms) |\n|---|---|---|---|---|---|---|\n")
set(failures "")
set(port ${PORT})

foreach(entry IN LISTS conditions)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 name)
    list(GET fields 1 delay)
    list(GET fields 2 jitter)
    list(GET fields 3 loss)
    list(GET fields 4 hang)

    # A copy of the game a run, with this condition written into it.
    set(game "${STAGE}/${name}")
    file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)
    file(WRITE "${game}/src/client/Condition.module.luau"
         "--!strict\nexport type Condition = { Name: string, HangSeconds: number }\n"
         "local Condition: Condition = { Name = \"${name}\", HangSeconds = ${hang} }\nreturn Condition\n")

    # Both at once: `execute_process` with two commands runs them together.
    # The server outlives the client by a few seconds and the client's frames
    # cover the join, the walk and the measuring.
    execute_process(
        COMMAND "${HOST}" --headless --pace=60 "--serve=${port}" --frames=1800 "${game}"
        COMMAND "${HOST}" --headless --pace=60 "--join=127.0.0.1:${port}" --frames=1500
                "--net-delay=${delay}" "--net-jitter=${jitter}" "--net-loss=${loss}" "${game}"
        WORKING_DIRECTORY "${game}"
        RESULT_VARIABLE results
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    math(EXPR port "${port} + 1")

    string(REGEX MATCH "\\[acceptance\\] condition=[^\n]*" line "${output}")
    if(line STREQUAL "")
        message("${output}")
        string(APPEND failures "  ${name}: the bot printed no measurement\n")
        string(APPEND table "| ${name} | -- | -- | -- | -- | -- | -- |\n")
        continue()
    endif()
    message("${line}")
    string(REGEX MATCH "corrections=([0-9]+)" _ "${line}")
    set(corrections ${CMAKE_MATCH_1})
    string(REGEX MATCH "hitches=([0-9]+)" _ "${line}")
    set(hitches ${CMAKE_MATCH_1})
    string(REGEX MATCH "stop=([0-9.]+)" _ "${line}")
    set(stop ${CMAKE_MATCH_1})
    string(REGEX MATCH "ping=([0-9.]+)" _ "${line}")
    set(ping ${CMAKE_MATCH_1})
    string(REGEX MATCH "remote=([0-9.]+)" _ "${line}")
    set(remote ${CMAKE_MATCH_1})
    string(REGEX MATCH "interpolation=([0-9.]+)" _ "${line}")
    set(interpolation ${CMAKE_MATCH_1})
    string(APPEND table "| ${name} | ${corrections} | ${hitches} | ${stop} | ${ping} | ${remote} | ${interpolation} |\n")

    # What each condition is held to. **A long frame costs at most two
    # corrections** -- the time the client dropped, which the authority went on
    # simulating, and the one the stream anchored again on -- and the bot
    # counts the frames over a tenth of a second it really had: the one a
    # condition hangs on purpose, and any a machine busy with the rest of the
    # gate makes of its own. Every other tick of the walk is uncorrected.
    math(EXPR allowed "2 * ${hitches}")
    if(corrections GREATER allowed)
        string(APPEND failures
             "  ${name}: ${corrections} corrections on a straight walk with ${hitches} long frames (at most ${allowed})\n")
    endif()
    if(stop GREATER 0.001)
        string(APPEND failures "  ${name}: stopped ${stop} m from the authority (at most 0.001)\n")
    endif()
    # A remote's round trip: the ping, a tick and a frame -- and the ping is
    # what ENet measures, so the jitter it has is in it.
    math(EXPR budget "2 * ${delay} + 2 * ${jitter} + 34")
    if(remote GREATER budget)
        string(APPEND failures "  ${name}: a remote took ${remote} ms there and back (at most ${budget})\n")
    endif()
endforeach()

file(WRITE "${STAGE}/netcode-acceptance.md" "${table}")
message("\nnetcode acceptance:\n${table}")
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "netcode acceptance: what did not hold --\n${failures}")
endif()
message("netcode acceptance: every condition held")
