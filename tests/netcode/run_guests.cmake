# The two-process guests gate (ADR 0205, protocol 48).
#
# A server, and a joiner with somebody else at its screen. The joiner seats a
# guest in the match, drives its own player to the right and the guest
# forwards -- by code, where a controller each would -- says a word as each,
# and gives the seat back. What the gate holds is that ONE connection is two
# players in every way that matters:
#
#   - two numbers, the guest's given by the authority and taken by the guest's
#     `Player` on the joiner;
#   - two intents a tick: each marker moved along its own axis and not the
#     other's, and a button that is the guest's alone counted for the guest
#     alone, for exactly the ticks it was held;
#   - two senders and two addressees: `FireServerFor(guest)` is heard from the
#     guest, and the answer to it is the guest's (`LocalClientReceived`);
#   - the guest's character is, on the joiner, where the authority has it;
#   - the guest leaves by itself, and the machine plays on.
#
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/guests> -DSTAGE=<dir>
#         -DPORT=<n> -P run_guests.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_guests.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)

execute_process(
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--serve=${PORT}" --frames=900 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=500 "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

set(failures "")
function(line_of out what)
    string(REGEX MATCH "\\[guests\\] ${what}[^\n]*" found "${output}")
    set(${out} "${found}" PARENT_SCOPE)
    if(found STREQUAL "")
        set(failures "${failures}  nothing said of: ${what}\n" PARENT_SCOPE)
    endif()
endfunction()
function(holds line text)
    string(FIND "${line}" "${text}" at)
    if(at EQUAL -1)
        set(failures "${failures}  not `${text}` in: ${line}\n" PARENT_SCOPE)
    endif()
endfunction()
# A number a line gives under a name, whole: `z=8.38` is 8.
function(whole_of out line name)
    string(REGEX MATCH "${name}=(-?[0-9]+)" _ "${line}")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# Two players of one connection, each with a number of its own.
line_of(seated "seated ")
holds("${seated}" "locals=2")
holds("${seated}" "players=2")
whole_of(firstId "${seated}" "first")
whole_of(guestId "${seated}" "guest")
if(firstId STREQUAL "" OR guestId STREQUAL "" OR firstId EQUAL guestId OR guestId GREATER 1000)
    string(APPEND failures "  the guest has no number of its own from the authority: ${seated}\n")
endif()
# The server's own account, which it leaves where the joiner reads it.
line_of(joinedFirst "server Joined1 user=${firstId} ")
line_of(joinedGuest "server Joined2 user=${guestId} players=2")

# Each said its own word, and was answered as itself.
line_of(heardFirst "server Heard1 user=${firstId} text=mine")
line_of(heardGuest "server Heard2 user=${guestId} text=theirs")
line_of(echoFirst "client heard as=first text=echo mine")
line_of(echoGuest "client heard as=guest text=echo theirs")

# Two intents a tick. The guest went forwards and never sideways, and pressed
# its button for a quarter of a second -- fifteen ticks, give or take the one
# a wait rounds to.
line_of(seatGuest "server Seat2 ")
holds("${seatGuest}" "user=${guestId} x=0.00 ")
whole_of(guestZ "${seatGuest}" "z")
whole_of(guestActs "${seatGuest}" "acts")
if(guestZ STREQUAL "" OR guestZ LESS 5)
    string(APPEND failures "  the guest's own intent did not move its marker: ${seatGuest}\n")
endif()
if(guestActs STREQUAL "" OR guestActs LESS 13 OR guestActs GREATER 17)
    string(APPEND failures "  the guest's button was held fifteen ticks and counted ${guestActs}\n")
endif()
# The machine's first player went right and never forwards, and heard none of
# the guest's button.
line_of(seatFirst "server Seat1 ")
holds("${seatFirst}" "user=${firstId} ")
holds("${seatFirst}" " z=0.00 acts=0 ")
whole_of(firstX "${seatFirst}" "x")
if(firstX STREQUAL "" OR firstX LESS 5)
    string(APPEND failures "  the first player's own intent did not move its marker: ${seatFirst}\n")
endif()
# And the guest left by itself: the server saw it go, and the first stay.
line_of(leftGuest "server Left2 user=${guestId} ")
line_of(leftFirst "server Left1 nothing")

# On the joiner, the guest's character is where the authority has it: along
# its own axis, some metres on.
line_of(sees "client sees ")
string(REGEX MATCH "guest=(-?[0-9]+)\\.[0-9],(-?[0-9]+)\\.[0-9]" _ "${sees}")
if(CMAKE_MATCH_1 STREQUAL "" OR NOT CMAKE_MATCH_1 EQUAL 0 OR CMAKE_MATCH_2 LESS 4)
    string(APPEND failures "  the joiner does not see its guest where the authority has it: ${sees}\n")
endif()

# The seat given back: one player here, and one in the match.
line_of(removed "removed=true")
line_of(after "after locals=1 players=1")
line_of(done "done")

string(FIND "${output}" "[warn]" warned)
string(FIND "${output}" "[error]" erred)
if(NOT warned EQUAL -1 OR NOT erred EQUAL -1)
    string(APPEND failures "  one of the two ends warned or erred\n")
endif()

message("${seated}
${heardFirst}
${heardGuest}
${echoGuest}
${seatGuest}
${seatFirst}
${sees}
${leftGuest}
${after}")

if(NOT failures STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "guests: a machine's second player was not a player of the match as it should be:\n${failures}")
endif()
message("guests: one connection, two players -- seated, driven, heard and answered each as itself, and one gone")
