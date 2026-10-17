# The netcode acceptance gate.
#
# A real server and a real client, two processes over the real transport, the
# client's link made worse by the link conditioner (`--net-delay`,
# `--net-jitter`, `--net-loss`) -- and the bot in
# `tests/netcode/acceptance` walking a predicted character a straight line for
# ten seconds, stopping, and measuring. One run a condition; the numbers come
# out as one table, and each is held to what the plan asks:
#
#   - a straight walk is not corrected (twice at most for a long frame: the
#     time the client dropped, and the stream anchored again);
#   - the second in which the walk BEGINS costs at most one correction on a
#     link that loses packets, and none on one that does not;
#   - stopped, the replica is where the authority is, to a millimetre;
#   - a remote's round trip is at most the ping, a tick and a frame;
#   - the ping is the link's.
#
# **And with a guest** (`-DGUEST=1`, ADR 0208): the bot seats a second player at
# its own machine and walks both. A machine that joined predicts a character
# for each of its players, so every line above is held for each of the two by
# itself, and two things more:
#
#   - the guest's character answers its stick in the tick the first player's
#     answers its own, or the one after;
#   - with the authority's word a metre off for the guest alone (`nudge`), the
#     guest is corrected once, by that metre, and the first player's character
#     is not: not counted, never a millimetre aside, and each tick's step the
#     step it was taking.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/acceptance> -DSTAGE=<dir>
#         -DPORT=<first port> [-DGUEST=1] -P run_netcode_acceptance.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_netcode_acceptance.cmake: -D${required}=... is required")
    endif()
endforeach()

# name; delay each way (ms); jitter (ms); loss (%); a frame to hang (s)
if(GUEST)
    # The four links of the plan, and the one-wrong-of-two on a clean one:
    # what a long frame does is the single player's gate's to hold.
    set(conditions
        "clean|0|0|0|0"
        "50ms|25|20|2|0"
        "150ms|75|20|2|0"
        "300ms|150|20|2|0"
        "nudge|0|0|0|0")
    set(guestLuau "true")
    set(title "netcode acceptance, with a guest")
    # Five seconds more: where each of the two stopped is asked of the
    # authority, eleven times each, and at 300 ms that is seven seconds.
    set(serverFrames 2100)
    set(clientFrames 1800)
else()
    set(conditions
        "clean|0|0|0|0"
        "50ms|25|20|2|0"
        "150ms|75|20|2|0"
        "300ms|150|20|2|0"
        "hang150|25|20|2|0.15"
        "hang400|25|20|2|0.4")
    set(guestLuau "false")
    set(title "netcode acceptance")
    set(serverFrames 1800)
    set(clientFrames 1500)
endif()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(table "| condition | corrections | long frames | as it began | stop (m) | ping (ms) | remote (ms) | interpolation (ms) |\n|---|---|---|---|---|---|---|---|\n")
set(guestTable "| condition | corrections: first, guest | as it began: first, guest | guest's stop (m) | ticks to answer the stick: first, guest |\n|---|---|---|---|---|\n")
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
    set(nudgeLuau "false")
    if(name STREQUAL "nudge")
        set(nudgeLuau "true")
    endif()
    file(WRITE "${game}/src/client/Condition.module.luau"
         "--!strict\nexport type Condition = { Name: string, HangSeconds: number, Guest: boolean, Nudge: boolean }\n"
         "local Condition: Condition = { Name = \"${name}\", HangSeconds = ${hang}, Guest = ${guestLuau}, "
         "Nudge = ${nudgeLuau} }\nreturn Condition\n")

    # Both at once: `execute_process` with two commands runs them together.
    # The server outlives the client by a few seconds and the client's frames
    # cover the join, the walk and the measuring. **The client draws nothing**
    # (`--rhi=null`): what is measured is the simulation and the link, and a
    # runner with no GPU -- CI's Linux one -- could not open the ordinary
    # device, so the client never ran and the bot printed nothing.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DOUT=server.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- "${HOST}" --headless --pace=60 "--serve=${port}" --frames=${serverFrames} "${game}"
        COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${port}"
                --frames=${clientFrames}
                "--net-delay=${delay}" "--net-jitter=${jitter}" "--net-loss=${loss}" --net-log-corrections "${game}"
        WORKING_DIRECTORY "${game}"
        RESULT_VARIABLE results
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    math(EXPR port "${port} + 1")

    string(REGEX MATCH "\\[acceptance\\] condition=[^\n]*" line "${output}")
    if(line STREQUAL "")
        message("${output}")
        string(APPEND failures "  ${name}: the bot printed no measurement\n")
        string(APPEND table "| ${name} | -- | -- | -- | -- | -- | -- | -- |\n")
        continue()
    endif()
    message("${line}")
    string(REGEX MATCH "corrections=([0-9]+)" _ "${line}")
    set(corrections ${CMAKE_MATCH_1})
    string(REGEX MATCH " hitches=([0-9]+)" _ "${line}")
    set(hitches ${CMAKE_MATCH_1})
    string(REGEX MATCH "start=([0-9]+)" _ "${line}")
    set(start ${CMAKE_MATCH_1})
    string(REGEX MATCH "startHitches=([0-9]+)" _ "${line}")
    set(startHitches ${CMAKE_MATCH_1})
    string(REGEX MATCH "stop=([0-9.]+)" _ "${line}")
    set(stop ${CMAKE_MATCH_1})
    string(REGEX MATCH "ping=([0-9.]+)" _ "${line}")
    set(ping ${CMAKE_MATCH_1})
    string(REGEX MATCH "remote=([0-9.]+)" _ "${line}")
    set(remote ${CMAKE_MATCH_1})
    string(REGEX MATCH "interpolation=([0-9.]+)" _ "${line}")
    set(interpolation ${CMAKE_MATCH_1})
    string(APPEND table "| ${name} | ${corrections} | ${hitches} | ${start} | ${stop} | ${ping} | ${remote} | ${interpolation} |\n")

    # What each condition is held to. **A long frame costs at most two
    # corrections** -- the time the client dropped, which the authority went on
    # simulating, and the one the stream anchored again on -- and the bot
    # counts the frames over three ticks both machines really had: the one a
    # condition hangs on purpose, and any a machine busy with the rest of the
    # gate makes of its own (a CI runner's server stalled and cost corrections
    # the client's own count never saw). **And one more where there was any**:
    # the link loses 2%, and a packet lost in the ticks the stream recovers in
    # costs one -- six runs of `hang150` on one machine took 0, 2, 0, 1, 2 and
    # 0, and the Linux tier once took 3. Every other tick of the walk is
    # uncorrected.
    set(slack 0)
    if(hitches GREATER 0)
        set(slack 1)
    endif()
    math(EXPR allowed "2 * ${hitches} + ${slack}")
    # **Each correction says what it disagreed about**
    # (`--net-log-corrections`): the client's log has a line for every one,
    # the landing at the join among them. One that should not have been is
    # printed with the rest, so a run that fails says why -- a count alone
    # was a failure nobody could act on, four times.
    string(REGEX MATCHALL "Corrected at tick [^\n]*" said "${output}")
    list(LENGTH said saidCount)
    # **The walk's first second**: the key going down is one intent,
    # and where the packets carrying it were lost or late the authority began
    # the walk a tick after the client did -- one correction, one tick's walk.
    # None where the link loses nothing.
    set(startAllowed 0)
    if(loss GREATER 0)
        set(startAllowed 1)
    endif()
    math(EXPR startAllowed "${startAllowed} + 2 * ${startHitches}")
    # **With a guest that is allowed a character, and held a character below**:
    # the two sticks are pushed in one tick and travel as one intent,
    # so the packet that is late is late for both, and each is corrected once
    # by its own tick's walk. Held here as the machine's total, that was two
    # against one -- the gate's own first run on a hosted runner, at 300 ms.
    if(NOT GUEST AND start GREATER startAllowed)
        string(APPEND failures
             "  ${name}: ${start} corrections as the walk began with ${startHitches} long frames (at most ${startAllowed})\n")
        foreach(line IN LISTS said)
            string(APPEND failures "      ${line}\n")
        endforeach()
    endif()
    if(corrections GREATER 0 AND saidCount LESS corrections)
        string(APPEND failures
             "  ${name}: ${corrections} corrections counted and ${saidCount} said in the log\n")
    endif()
    # **With a guest, each of the two is held by itself.** The machine's count
    # is the two together, and what is allowed is allowed a character: a long
    # frame drops time for both, and a lost packet as the walk begins is late
    # for both. The nudge is one more for the guest, and that one is asked for.
    if(GUEST)
        set(asked 0)
        if(name STREQUAL "nudge")
            set(asked 1)
        endif()
        foreach(field firstCorrections guestCorrections firstStart guestStart firstStick guestStick)
            string(REGEX MATCH " ${field}=(-?[0-9]+)" _ "${line}")
            set(${field} "${CMAKE_MATCH_1}")
            if("${${field}}" STREQUAL "" OR "${${field}}" LESS 0)
                string(APPEND failures "  ${name}: the bot said nothing of ${field} -- is each of the two predicted?\n")
                set(${field} 0)
            endif()
        endforeach()
        string(REGEX MATCH "guestStop=([0-9.]+)" _ "${line}")
        set(guestStop ${CMAKE_MATCH_1})
        string(APPEND guestTable
               "| ${name} | ${firstCorrections}, ${guestCorrections} | ${firstStart}, ${guestStart} | ${guestStop} | ${firstStick}, ${guestStick} |\n")
        math(EXPR guestAllowed "${allowed} + ${asked}")
        if(firstCorrections GREATER allowed)
            string(APPEND failures
                 "  ${name}: the first player's character took ${firstCorrections} corrections on a straight walk with ${hitches} long frames (at most ${allowed})\n")
        endif()
        if(guestCorrections GREATER guestAllowed)
            string(APPEND failures
                 "  ${name}: the guest's character took ${guestCorrections} corrections on a straight walk with ${hitches} long frames (at most ${guestAllowed})\n")
        endif()
        if(firstStart GREATER startAllowed OR guestStart GREATER startAllowed)
            string(APPEND failures
                 "  ${name}: as the walk began the first took ${firstStart} corrections and the guest ${guestStart} (at most ${startAllowed} each)\n")
        endif()
        if(firstCorrections GREATER allowed OR guestCorrections GREATER guestAllowed
           OR firstStart GREATER startAllowed OR guestStart GREATER startAllowed)
            foreach(each IN LISTS said)
                string(APPEND failures "      ${each}\n")
            endforeach()
        endif()
        if(guestStop GREATER 0.001)
            string(APPEND failures "  ${name}: the guest stopped ${guestStop} m from the authority (at most 0.001)\n")
        endif()
        # **The guest answers its stick when the first player does.** Both are
        # predicted, so both move in the tick the stick does -- whatever the
        # link is. A guest that is not predicted answers a round trip later:
        # ten ticks at 150 ms, nineteen at 300.
        math(EXPR apart "${guestStick} - ${firstStick}")
        if(apart LESS -1 OR apart GREATER 1 OR firstStick GREATER 3)
            string(APPEND failures
                 "  ${name}: the first player's character answered its stick in ${firstStick} ticks and the guest's in ${guestStick} (the same, within one, and at most 3)\n")
        endif()
        if(name STREQUAL "nudge")
            string(REGEX MATCH "nudgeGuest=(-?[0-9]+) nudgeFirst=(-?[0-9]+) nudgeSize=([0-9.]+) aside=([0-9.]+) stepLeast=([0-9.]+) stepMost=([0-9.]+)" _ "${line}")
            set(nudgeGuest "${CMAKE_MATCH_1}")
            set(nudgeFirst "${CMAKE_MATCH_2}")
            set(nudgeSize "${CMAKE_MATCH_3}")
            set(aside "${CMAKE_MATCH_4}")
            set(stepLeast "${CMAKE_MATCH_5}")
            set(stepMost "${CMAKE_MATCH_6}")
            if(nudgeGuest STREQUAL "")
                string(APPEND failures "  ${name}: the bot said nothing of the nudge\n")
            else()
                string(APPEND guestTable
                       "\nThe authority's word a metre off for the guest alone: the guest corrected ${nudgeGuest} time(s), by ${nudgeSize} m; the first player's character ${nudgeFirst} time(s), ${aside} m aside at most, each tick's step ${stepLeast} to ${stepMost} m.\n")
                if(NOT nudgeGuest EQUAL 1 OR nudgeSize LESS 0.9 OR nudgeSize GREATER 1.1)
                    string(APPEND failures
                         "  ${name}: the guest's character was corrected ${nudgeGuest} time(s) by ${nudgeSize} m (once, by the metre it was put aside)\n")
                endif()
                # Eight metres a second at sixty ticks: 0.1333 m a tick.
                if(NOT nudgeFirst EQUAL 0 OR aside GREATER 0.0001 OR stepLeast LESS 0.133 OR stepMost GREATER 0.134)
                    string(APPEND failures
                         "  ${name}: the first player's character was moved by the guest's correction: corrected ${nudgeFirst} time(s), ${aside} m aside, a tick's step ${stepLeast} to ${stepMost} m (none, none, and 0.1333)\n")
                endif()
            endif()
        endif()
    elseif(corrections GREATER allowed)
        string(APPEND failures
             "  ${name}: ${corrections} corrections on a straight walk with ${hitches} long frames (at most ${allowed})\n")
        foreach(line IN LISTS said)
            string(APPEND failures "      ${line}\n")
        endforeach()
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

if(GUEST)
    string(APPEND table "\n${guestTable}")
endif()
file(WRITE "${STAGE}/netcode-acceptance.md" "${table}")
message("\n${title}:\n${table}")
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "${title}: what did not hold --\n${failures}")
endif()
message("${title}: every condition held")
