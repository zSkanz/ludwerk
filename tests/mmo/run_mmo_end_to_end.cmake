# **An online game, end to end** (MMO Phase C's proof), in real processes as
# a game ships: a manager with three worlds -- the town, the fields, the
# harbor -- and two kinds of instance, and three players' machines.
#
# Each player makes an account at the front door and signs in with it. The
# town makes a guild of the three and sends Bob to the fields and Cyd to the
# harbor; Ana says a line in the guild's channel, and each of the three hears
# it once, in three worlds. Cyd is sent to a dungeon of their own; Ana and
# Bob, from two worlds, queue for a duel and are matched into one arena. The
# two trade there until the manager -- the process that holds the game's
# data -- is ended at a transaction, after it is written and before its
# answer goes back. A second run of the servers counts what the traders
# hold: nothing duplicated, nothing lost.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_mmo_end_to_end.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(game "${WORKDIR}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)
set(apart "${CMAKE_CURRENT_LIST_DIR}/../netcode/apart.cmake")

# **The servers**, on ports of a run's own: a world of the first run may
# still be closing on its port when the second starts.
function(servers door)
    math(EXPR first "${door} + 1")
    math(EXPR last "${door} + 6")
    file(WRITE "${WORKDIR}/servers.toml"
         "[manager]\nport = ${door}\nworld_ports = \"${first}-${last}\"\n\n"
         "[accounts]\nenabled = true\nregister = true\n\n"
         "[[world]]\nname = \"town\"\nscene = \"scenes/town.scene.json\"\nentry = true\n\n"
         "[[world]]\nname = \"fields\"\nscene = \"scenes/fields.scene.json\"\n\n"
         "[[world]]\nname = \"harbor\"\nscene = \"scenes/harbor.scene.json\"\n\n"
         "[[instance]]\ntemplate = \"dungeon\"\nscene = \"scenes/dungeon.scene.json\"\nmax_instances = 2\n"
         "idle_close_seconds = 300\n\n"
         "[[instance]]\ntemplate = \"arena\"\nscene = \"scenes/arena.scene.json\"\nmax_instances = 2\n"
         "idle_close_seconds = 300\n")
endfunction()

function(read_logs out pattern)
    file(GLOB found "${WORKDIR}/logs/${pattern}")
    set(text "")
    foreach(one IN LISTS found)
        file(READ "${one}" said)
        string(APPEND text "${said}")
    endforeach()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# **The first run**: the manager ended at the arena's 25th transaction --
# the gold it starts with, then its 24th trade -- after it is written.
servers(${PORT})
set(client --headless --rhi=null --pace=60 --frames=10800 --no-xr "--exit-on-line=client in ")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -DOUT=manager.out -P "${apart}" --
            "${HOST}" "--manager=${WORKDIR}/servers.toml" --exit-at-transaction=25 --exit-after-commit
    COMMAND "${CMAKE_COMMAND}" -DOUT=ana.out -P "${apart}" --
            "${HOST}" ${client} "--scene-data={\"door\":\"127.0.0.1:${PORT}\",\"name\":\"ana\"}" "${game}"
    COMMAND "${CMAKE_COMMAND}" -DOUT=bob.out -P "${apart}" --
            "${HOST}" ${client} "--scene-data={\"door\":\"127.0.0.1:${PORT}\",\"name\":\"bob\"}" "${game}"
    COMMAND "${HOST}" ${client} "--scene-data={\"door\":\"127.0.0.1:${PORT}\",\"name\":\"cyd\"}" "${game}"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 300
    RESULT_VARIABLE results
    OUTPUT_VARIABLE cyd
    ERROR_VARIABLE cyd)

set(ana "")
set(bob "")
if(EXISTS "${WORKDIR}/ana.out")
    file(READ "${WORKDIR}/ana.out" ana)
endif()
if(EXISTS "${WORKDIR}/bob.out")
    file(READ "${WORKDIR}/bob.out" bob)
endif()
read_logs(town "town/town.log")
read_logs(fields "fields/fields.log")
read_logs(harbor "harbor/harbor.log")
read_logs(instances "i-*/i-*.log")
set(manager "")
foreach(log "${WORKDIR}/manager.out" "${WORKDIR}/manager.out.err")
    if(EXISTS "${log}")
        file(READ "${log}" said)
        string(APPEND manager "${said}")
    endif()
endforeach()
set(said "--- Ana's machine:\n${ana}\n--- Bob's machine:\n${bob}\n--- Cyd's machine:\n${cyd}\n--- the manager:\n"
         "${manager}\n--- the town:\n${town}\n--- the fields:\n${fields}\n--- the harbor:\n${harbor}\n"
         "--- the instances:\n${instances}")

function(expect text what why)
    string(FIND "${text}" "${what}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${why} (${results}): no `${what}`\n${said}")
    endif()
endfunction()

# **Login**: an account made, and signed in with.
foreach(name ana bob cyd)
    expect("${${name}}" "[e2e] ${name} registered ok=true" "${name} did not make an account")
    expect("${${name}}" "[e2e] ${name} signed in ok=true" "${name} did not sign in")
endforeach()
# **Travel, and a guild across three worlds**: its line heard once by each.
expect("${town}" "[e2e] town guild members=3" "the town did not make a guild of the three")
expect("${fields}" "[e2e] fields arrived bob" "Bob did not travel to the fields")
expect("${harbor}" "[e2e] harbor arrived cyd" "Cyd did not travel to the harbor")
foreach(name ana bob cyd)
    string(REGEX MATCHALL "\\[e2e\\] ${name} heard guild:ana:for the wardens" lines "${${name}}")
    list(LENGTH lines heard)
    if(NOT heard EQUAL 1)
        message(FATAL_ERROR "${name} heard the guild's line ${heard} times, not once (${results}):\n${said}")
    endif()
endforeach()
# **An instance**: Cyd's dungeon.
expect("${cyd}" "[e2e] cyd client in dungeon" "Cyd was not sent to a dungeon")
expect("${instances}" "[e2e] dungeon arrived=" "the dungeon did not see Cyd arrive")
# **Matchmaking into an arena**, from two worlds.
expect("${town}" "[e2e] town queued ana=true" "Ana did not queue")
expect("${fields}" "[e2e] fields queued bob=true" "Bob did not queue")
expect("${ana}" "[e2e] ana client in arena" "Ana was not sent to the arena")
expect("${bob}" "[e2e] bob client in arena" "Bob was not sent to the arena")
string(REGEX MATCHALL "\\[e2e\\] arena arrived=[^\n]+" arrivals "${instances}")
list(LENGTH arrivals arrived)
if(NOT arrived EQUAL 2)
    message(FATAL_ERROR "the arena saw ${arrived} players arrive, not two (${results}):\n${said}")
endif()
# **The trade, and the manager ended in the middle of one.**
expect("${instances}" "[e2e] arena trading swords=5 gold=1000" "the arena did not begin to trade")
expect("${instances}" "[e2e] arena traded=" "the arena made no trade")
expect("${cyd}" "ended with 86" "the manager was not ended at its transaction")

# **The second run**: the servers again, from what the first left.
math(EXPR again "${PORT} + 10")
servers(${again})
file(REMOVE_RECURSE "${WORKDIR}/logs")
execute_process(
    COMMAND "${HOST}" "--manager=${WORKDIR}/servers.toml" "--exit-on-line=[e2e] count"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 120
    RESULT_VARIABLE recounted
    OUTPUT_VARIABLE recount
    ERROR_VARIABLE recount)
read_logs(town "town/town.log")
if(NOT town MATCHES "\\[e2e\\] count swords=5 gold=1000")
    message(FATAL_ERROR "after the manager was ended in a trade, the traders do not hold five swords and a thousand "
                        "gold (${recounted}):\n${recount}\n--- the town:\n${town}\n--- the first run:\n${said}")
endif()
message("an online game, end to end: signed in, travelled, a guild heard in three worlds, a dungeon, a match, "
        "and a trade cut by the manager's end with nothing duplicated or lost")
