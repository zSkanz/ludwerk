# The netcode gate of a driven vehicle (ADR 0213).
#
# A real server and a real client over the real transport, the client's link
# held, jittered and lost by the link conditioner, and the bot in
# `tests/netcode/vehicle` driving a car its server made it the `Driver` of:
# a straight line, a corner, a stop on the brake. The car is predicted on the
# driver's machine as the character is, so each condition is held to:
#
#   - a straight and a corner are not corrected (one where the link lost the
#     tick a change began, and one more for each long frame either machine
#     had);
#   - stopped, the replica's car is where the authority's is, to a centimetre;
#   - the furthest any correction moved it is in the table: what a
#     correction looks like when there is one;
#   - the car as it is drawn never jumps: frame to frame, its drawn place
#     (`GetRenderCFrame`) is never off the line of its last two frames by more
#     than two centimetres beyond what the authority's own car did over the
#     same stretch -- a crash stops a car in a tick everywhere; a correction
#     is drawn gliding. Its simulated place's jump is printed beside it: what
#     the glide hides;
#   - and the car is not left apart from the authority's: the furthest it was
#     from it at an answer (`PredictedDrift`), over a drive, stays under the
#     vehicle's correction, three centimetres;
#   - the car answers its stick in the tick the character answers its own,
#     or the one after: a car drawn from snapshots would answer a round trip
#     late, which at 150 ms is nine ticks.
#
# **And into a parked car** (`touch`): the authority parks a car twenty metres
# ahead and the bot drives into it at a walking pace. Two cars touching are
# corrected at most a few times, and the stop after is the authority's. The
# parked car is nobody's: at rest, both machines know what it will do.
#
# **The measure is not blind** (`nudge`): a drive in which the authority moves
# the car half a metre aside, once, in the straight -- nothing the driver's
# machine could predict. The straight must be corrected, and the largest
# correction must be that half metre. A run that said nought here would be a
# measure that cannot see a correction, and every nought above with it.
#
# **A stall** (`stall`, 150 ms): the driver's machine stops for 200 ms in the
# straight. It catches the ticks up and goes on as before: no correction, and
# the car drawn no further than the glide allows.
#
# **Two drivers** (`duel`, 150 ms): two clients, their cars facing each other,
# each driving at the other with a wheel that wanders from a seed of its own.
# Neither machine knows where the other's wheel goes next, so corrections are
# expected while they touch; what is held is that they met, and what is
# printed is how many and how big, from each driver's machine.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/vehicle> -DSTAGE=<dir>
#         -DPORT=<first port> -P run_netcode_vehicle.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_netcode_vehicle.cmake: -D${required}=... is required")
    endif()
endforeach()

# name; delay each way (ms); jitter (ms); loss (%); what is driven; clients
set(conditions
    "clean|0|0|0|drive|1"
    "50ms|25|20|2|drive|1"
    "150ms|75|20|2|drive|1"
    "300ms|150|20|2|drive|1"
    "touch|25|20|2|park|1"
    "nudge|25|20|2|nudge|1"
    "stall|75|20|2|stall|1"
    "duel|75|20|2|duel|2")

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(table "| condition | seat | as it began | straight | corner | stopping | contact | between them | largest correction (m) | drawn jump (mm) | simulated jump (mm) | the authority's own jump (mm) | drift from the authority, mean / max (mm) | long frames | stop (m) | closest (m) | ticks to answer: car, character | ping (ms) |\n|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n")
set(failures "")
set(port ${PORT})

foreach(entry IN LISTS conditions)
    string(REPLACE "|" ";" fields "${entry}")
    list(GET fields 0 name)
    list(GET fields 1 delay)
    list(GET fields 2 jitter)
    list(GET fields 3 loss)
    list(GET fields 4 mode)
    list(GET fields 5 clients)

    set(game "${STAGE}/${name}")
    file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)
    file(WRITE "${game}/src/shared/Condition.luau"
         "--!strict\nexport type Condition = { Name: string, Mode: string }\n"
         "local Condition: Condition = { Name = \"${name}\", Mode = \"${mode}\" }\nreturn Condition\n")

    # The authority and its clients at once; the clients draw nothing.
    set(client "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${port}" --frames=1800
               "--net-delay=${delay}" "--net-jitter=${jitter}" "--net-loss=${loss}" --net-log-corrections "${game}")
    if(clients EQUAL 2)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -DOUT=server.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- "${HOST}" --headless --pace=60 "--serve=${port}" --frames=2100 "${game}"
            COMMAND "${CMAKE_COMMAND}" -DOUT=client-1.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- ${client}
            COMMAND ${client}
            WORKING_DIRECTORY "${game}"
            RESULT_VARIABLE results
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output)
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -DOUT=server.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- "${HOST}" --headless --pace=60 "--serve=${port}" --frames=2100 "${game}"
            COMMAND ${client}
            WORKING_DIRECTORY "${game}"
            RESULT_VARIABLE results
            OUTPUT_VARIABLE output
            ERROR_VARIABLE output)
    endif()
    math(EXPR port "${port} + 1")

    # **From each machine's log**: the processes are a pipeline, and only
    # the last one's printing reaches this script whole.
    set(lines "")
    file(GLOB logs "${game}/engine*.log")
    foreach(log IN LISTS logs)
        file(STRINGS "${log}" found REGEX "\\[vehicle\\] condition=")
        foreach(one IN LISTS found)
            string(REGEX MATCH "\\[vehicle\\] condition=.*" one "${one}")
            list(APPEND lines "${one}")
        endforeach()
    endforeach()
    list(LENGTH lines said)
    if(NOT said EQUAL clients)
        message("${output}")
        string(APPEND failures "  ${name}: ${said} of ${clients} bots printed a measurement\n")
        string(APPEND table
               "| ${name} | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- | -- |\n")
        continue()
    endif()

    foreach(line IN LISTS lines)
        message("${line}")
        foreach(key seat start straight corner stopping contact hitches carTicks walkTicks corrections)
            string(REGEX MATCH " ${key}=(-?[0-9]+)" _ "${line}")
            set(${key} ${CMAKE_MATCH_1})
        endforeach()
        foreach(key stop ping closest largest drawnJump simJump driftMean driftMax driftBetween otherDrift
                    authorityJump)
            string(REGEX MATCH " ${key}=([0-9.a-z]+)" _ "${line}")
            set(${key} ${CMAKE_MATCH_1})
        endforeach()
        foreach(key otherPart car)
            string(REGEX MATCH " ${key}=([^ ]+)" _ "${line}")
            set(${key} ${CMAKE_MATCH_1})
        endforeach()
        # **The corrections between the measured stretches**: the drive's
        # whole count less the stretches'. They count against the stretch
        # rules below, as if they had fallen inside one.
        math(EXPR outside "${corrections} - ${start} - ${straight} - ${corner} - ${stopping} - ${contact}")
        if(closest STREQUAL "inf")
            set(closest "--")
        endif()
        # The largest correction in whole millimetres, for the glide's check.
        string(REGEX REPLACE "^0*([0-9]*)\\.([0-9][0-9][0-9]).*$" "\\1\\2" largestMillimetres "${largest}")
        if(largestMillimetres STREQUAL "")
            set(largestMillimetres 0)
        endif()
        math(EXPR largestMillimetres "${largestMillimetres} + 0")
        string(APPEND table
               "| ${name} | ${seat} | ${start} | ${straight} | ${corner} | ${stopping} | ${contact} | ${outside} | ${largest} | ${drawnJump} | ${simJump} | ${authorityJump} | ${driftMean} / ${driftMax} | ${hitches} | ${stop} | ${closest} | ${carTicks}, ${walkTicks} | ${ping} |\n")

        # A change begun in a tick the link lost costs one; a long frame on
        # either machine costs up to two.
        math(EXPR allowed "1 + 2 * ${hitches}")
        if(mode STREQUAL "park")
            math(EXPR allowedContact "3 + ${allowed}")
            math(EXPR touched "${contact} + ${outside}")
            if(touched GREATER allowedContact)
                string(APPEND failures "  ${name}: two cars touching took ${touched} corrections "
                       "(${outside} between the stretches)\n")
            endif()
        elseif(mode STREQUAL "drive")
            math(EXPR driven "${straight} + ${corner} + ${outside}")
            if(driven GREATER allowed)
                string(APPEND failures "  ${name}: a straight and a corner took ${driven} corrections "
                       "(${outside} between the stretches)\n")
            endif()
        elseif(mode STREQUAL "stall")
            # **A stall is caught up, not dropped** (`stallCatchUpSeconds`):
            # the machine stopped for 200 ms runs the twelve ticks it owes and
            # goes on predicting. Not one correction, anywhere in the drive --
            # what it dropped it used to be put at, 1.4 m on.
            math(EXPR driven "${straight} + ${corner} + ${outside}")
            if(driven GREATER 0)
                string(APPEND failures "  ${name}: a stall of 200 ms took ${driven} corrections\n")
            endif()
        elseif(mode STREQUAL "nudge")
            # Half a metre aside, seen as half a metre.
            if(straight LESS 1)
                string(APPEND failures "  ${name}: the car moved aside by the authority was never corrected -- "
                       "the measure is blind\n")
            endif()
            if(largest LESS 0.3 OR largest GREATER 0.8)
                string(APPEND failures "  ${name}: a half-metre move was seen as a correction of ${largest} m\n")
            endif()
            # **And drawn gliding there**: a fifth of what is left a tick
            # (`VehicleVisualDecayPerTick`), so the half metre is drawn as a
            # tenth of a metre in its first tick, never as the half metre --
            # the one row where the glide is all there is to see, and the
            # check that holds it.
            string(REGEX REPLACE "\\..*" "" nudgeDrawn "${drawnJump}")
            math(EXPR glideMost "${largestMillimetres} / 4 + 10")
            if(nudgeDrawn GREATER glideMost)
                string(APPEND failures "  ${name}: a ${largest} m correction was drawn as a ${drawnJump} mm jump -- "
                       "it did not glide\n")
            endif()
        endif()
        # Two cars that touched: centre to centre, less than a car's length
        # and a little.
        if(mode STREQUAL "park" OR mode STREQUAL "duel")
            if(closest STREQUAL "--" OR closest GREATER 4.3)
                string(APPEND failures "  ${name} (seat ${seat}): the cars never met (closest ${closest} m)\n")
            endif()
        endif()
        # **No visible snap**: the car drawn, frame to frame, never off the
        # line its last two frames drew by more than two centimetres -- a
        # correction slides (`VisualDecayPerTick`) rather than jumps. Not the
        # nudge's: half a metre moved aside is meant to be seen.
        # A crash stops a car in a tick on every machine: what is held is
        # what this machine draws beyond what the authority's own car did.
        # **In a duel, beyond what either machine's physics did**: each
        # driver's machine crashes its own car into the other car where it
        # sees it -- a remote car, drawn where it was a ping ago -- so its
        # crash is not the authority's, and neither bounds the other (49 to
        # 111 mm the authority's, over the runs). What the picture must not
        # do is snap on top of the physics: the drawn car against the larger
        # of the two crashes. The correction itself is held below.
        if(NOT mode STREQUAL "nudge")
            string(REGEX REPLACE "\\..*" "" drawnWhole "${drawnJump}")
            string(REGEX REPLACE "\\..*" "" authorityWhole "${authorityJump}")
            string(REGEX REPLACE "\\..*" "" simulatedWhole "${simJump}")
            set(physicsWhole ${authorityWhole})
            set(physicsSaid "the authority's own ${authorityJump}")
            if(mode STREQUAL "duel" AND simulatedWhole GREATER authorityWhole)
                set(physicsWhole ${simulatedWhole})
                set(physicsSaid "this machine's own simulated ${simJump}")
            endif()
            math(EXPR beyond "${drawnWhole} - ${physicsWhole}")
            # **What a re-simulation moved, drawn gliding**: an answer steps
            # the car again from the authority's state, and on a loaded link
            # that lands it a few centimetres from where it was predicted (81
            # mm at 300 ms with every core busy). The glide draws a fifth of
            # it in the first tick, so a quarter of the simulated jump and a
            # centimetre is what the picture may add -- and never less than two
            # centimetres. A broken glide draws the whole jump, and the nudge
            # holds that.
            math(EXPR snapMost "${simulatedWhole} / 4 + 10")
            if(snapMost LESS 20)
                set(snapMost 20)
            endif()
            if(beyond GREATER snapMost)
                string(APPEND failures "  ${name} (seat ${seat}): the car was drawn jumping ${drawnJump} mm in a tick, "
                       "${beyond} mm beyond ${physicsSaid}, past the ${snapMost} its glide may add\n")
            endif()
        endif()
        # **A duel's corrections are small**: two cars meeting at 150 ms are
        # corrected where each machine's crash parts from the authority's, by
        # a few centimetres (31 to 46 mm over the runs), never by a car's
        # width.
        if(mode STREQUAL "duel" AND largest GREATER 0.1)
            string(APPEND failures "  ${name} (seat ${seat}): a correction of ${largest} m in a duel\n")
        endif()
        # **The car is not left apart from the authority's**: under its
        # correction and pulled back each answer, the furthest the driven car
        # itself was from it stays under the correction -- on every row but
        # the nudge (moved half a metre on purpose) and the duel (corrected
        # past it by a crash, and held to 10 cm below). **Two cars touching
        # are corrected by design**, and a correction fires at that very
        # distance: there it is the car's drift in the ticks no correction
        # answered. The parked car's own is printed, never held.
        if(mode STREQUAL "park")
            if(driftBetween GREATER 30)
                string(APPEND failures "  ${name}: the car (${car}) drifted ${driftBetween} mm from the authority's "
                       "between corrections (the most of any other part: ${otherDrift} mm, ${otherPart})\n")
            endif()
        elseif(NOT mode STREQUAL "nudge" AND NOT mode STREQUAL "duel" AND driftMax GREATER 30)
            string(APPEND failures "  ${name}: the car (${car}) drifted ${driftMax} mm from the authority's\n")
        endif()
        # **And a ceiling the relative rules cannot raise**: on a clean link
        # the car steps where the authority's does, a millimetre or two a
        # tick off its line (1.7 to 1.8 measured), so the simulated jump there
        # stays under five.
        if(name STREQUAL "clean")
            string(REGEX REPLACE "\\..*" "" cleanSimulated "${simJump}")
            if(cleanSimulated GREATER_EQUAL 5)
                string(APPEND failures "  clean: the car's simulated place jumped ${simJump} mm on a clean link\n")
            endif()
        endif()
        if(NOT mode STREQUAL "duel" AND stop GREATER 0.01)
            string(APPEND failures "  ${name}: stopped ${stop} m from where the authority stopped\n")
        endif()
        if(carTicks LESS 0)
            string(APPEND failures "  ${name}: the car never answered its stick\n")
        elseif(walkTicks GREATER_EQUAL 0)
            math(EXPR late "${carTicks} - ${walkTicks}")
            if(late GREATER 1)
                string(APPEND failures "  ${name}: the car answered ${late} ticks after the character\n")
            endif()
        endif()
    endforeach()
endforeach()

file(WRITE "${STAGE}/table.md" "# netcode, a driven vehicle\n\n${table}")
message("${table}")
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "the driven vehicle is not held to the plan:\n${failures}")
endif()
