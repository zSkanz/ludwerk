# The game's Nakama (ADR 0242), started by `scripts/localgate.ps1 -Only
# nakama` and named here: a manager whose front door takes Nakama's sessions
# and whose data Nakama's storage keeps, through the integration's module; two
# worlds and a player who signs in to Nakama, turns the session into a ticket
# and joins. The town makes a thousand trades, both worlds count a shared
# value at once, and a record changed in one is loaded in the other -- all in
# Nakama. Then the same transaction sent twice to the module: done once.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT NAKAMA TWICE SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_nakama.cmake needs -D${required}=")
    endif()
endforeach()
if("$ENV{ENG_NAKAMA_HTTP_KEY}" STREQUAL "")
    message(FATAL_ERROR "ENG_NAKAMA_HTTP_KEY is not set: the stage that starts Nakama sets it")
endif()
find_program(ENG_LUTE_EXECUTABLE lute)
if(NOT ENG_LUTE_EXECUTABLE)
    message(FATAL_ERROR "the check of a request sent twice is Lute, and `lute` is not on PATH")
endif()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(game "${WORKDIR}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)
math(EXPR first "${PORT} + 1")
math(EXPR last "${PORT} + 2")
file(WRITE "${WORKDIR}/servers.toml"
     "[manager]\nport = ${PORT}\nworld_ports = \"${first}-${last}\"\n\n"
     "[accounts]\nenabled = true\nregister = false\n\n"
     "[nakama]\nurl = \"${NAKAMA}\"\nhttp_key_env = \"ENG_NAKAMA_HTTP_KEY\"\n\n"
     "[data]\nbackend = \"nakama\"\n\n"
     "[[world]]\nname = \"town\"\nscene = \"scenes/town.scene.json\"\nentry = true\n\n"
     "[[world]]\nname = \"fields\"\nscene = \"scenes/fields.scene.json\"\n")
# A device of this run's own: an account Nakama has not seen.
string(RANDOM LENGTH 16 ALPHABET "0123456789abcdef" device)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -DOUT=manager.out -P "${CMAKE_CURRENT_LIST_DIR}/../netcode/apart.cmake" --
            "${HOST}" "--manager=${WORKDIR}/servers.toml" "--exit-on-line=[nakama] done"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 --frames=21600
            "--scene-data={\"door\":\"127.0.0.1:${PORT}\",\"device\":\"engine-test-${device}\"}"
            "--exit-on-line=[nakama] client account=" "${game}"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 420
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

set(town "")
set(fields "")
if(EXISTS "${WORKDIR}/logs/town/town.log")
    file(READ "${WORKDIR}/logs/town/town.log" town)
endif()
if(EXISTS "${WORKDIR}/logs/fields/fields.log")
    file(READ "${WORKDIR}/logs/fields/fields.log" fields)
endif()
set(said "--- the player's machine:\n${output}\n--- the town:\n${town}\n--- the fields:\n${fields}")
if(NOT output MATCHES "\\[nakama\\] signed in to nakama=true +session=true")
    message(FATAL_ERROR "the player did not sign in to Nakama (${results}):\n${said}")
endif()
if(NOT output MATCHES "\\[nakama\\] client account=nakama:[0-9a-f-]+ level=7")
    message(FATAL_ERROR "the player was not known by their Nakama account, with the record carried (${results}):\n${said}")
endif()
if(NOT town MATCHES "\\[nakama\\] town traded=1000")
    message(FATAL_ERROR "the town did not make its thousand trades in Nakama (${results}):\n${said}")
endif()
if(NOT fields MATCHES "\\[nakama\\] done level=7 count=100 swords=5 gold=1000")
    message(FATAL_ERROR "Nakama did not keep the count and the goods as they must be (${results}):\n${said}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E chdir "${SOURCE}" "${ENG_LUTE_EXECUTABLE}" "${TWICE}"
            "--url=${NAKAMA}/v2/rpc/engine_data?unwrap&http_key=$ENV{ENG_NAKAMA_HTTP_KEY}" "--owner=twice-${device}"
    TIMEOUT 60
    RESULT_VARIABLE twiceResults
    OUTPUT_VARIABLE twiceOutput
    ERROR_VARIABLE twiceOutput)
if(NOT twiceOutput MATCHES "\\[twice\\] once:")
    message(FATAL_ERROR "the same transaction sent twice to Nakama was not done once (${twiceResults}):\n${twiceOutput}")
endif()
message("the game's Nakama signs its players in and keeps its records, trades and shared values, a request once")
