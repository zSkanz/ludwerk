# The data service against a game's own backend (ADR 0238 §5): a manager with
# `[data] backend` naming the reference backend, two worlds and a player. The
# town makes a thousand trades of a sword for its price, both worlds count a
# shared value at once, and a record changed in the town is the one the
# fields load -- all of it through the backend. Then the same transaction
# sent twice, as the manager does on a lost answer: done once.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT BACKEND_PORT BACKEND TWICE SOURCE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_backend.cmake needs -D${required}=")
    endif()
endforeach()
find_program(ENG_LUTE_EXECUTABLE lute)
if(NOT ENG_LUTE_EXECUTABLE)
    message(FATAL_ERROR "the reference backend is Lute, and `lute` is not on PATH")
endif()
# Lute is the repository's pinned one (rokit), found from its root: run from
# the test's folder, the shim finds no manifest and refuses.
set(lute "${CMAKE_COMMAND}" -E chdir "${SOURCE}" "${ENG_LUTE_EXECUTABLE}")

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(game "${WORKDIR}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)
math(EXPR first "${PORT} + 1")
math(EXPR last "${PORT} + 2")
file(WRITE "${WORKDIR}/servers.toml"
     "[manager]\nport = ${PORT}\nworld_ports = \"${first}-${last}\"\n\n"
     "[data]\nbackend = \"http://127.0.0.1:${BACKEND_PORT}\"\n\n"
     "[[world]]\nname = \"town\"\nscene = \"scenes/town.scene.json\"\nentry = true\n\n"
     "[[world]]\nname = \"fields\"\nscene = \"scenes/fields.scene.json\"\n")

# The backend, which ends a few seconds after its last request; the manager,
# which ends at the fields' last line; the player's machine, at its own.
execute_process(
    COMMAND ${lute} "${BACKEND}" "--port=${BACKEND_PORT}" --idle-exit=5
    COMMAND "${CMAKE_COMMAND}" -DOUT=manager.out -P "${CMAKE_CURRENT_LIST_DIR}/../netcode/apart.cmake" --
            "${HOST}" "--manager=${WORKDIR}/servers.toml" "--exit-on-line=[backend] done"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=14400
            "--exit-on-line=[backend] client level=" "${game}"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 300
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
set(said "--- the backend and the player's machine:\n${output}\n--- the town:\n${town}\n--- the fields:\n${fields}")
if(NOT town MATCHES "\\[backend\\] town traded=1000")
    message(FATAL_ERROR "the town did not make its thousand trades through the backend (${results}):\n${said}")
endif()
if(NOT fields MATCHES "\\[backend\\] done level=7 count=100 swords=5 gold=1000")
    message(FATAL_ERROR "the fields did not find the record, the count and the goods as they must be (${results}):\n${said}")
endif()
if(NOT output MATCHES "\\[backend\\] client level=7")
    message(FATAL_ERROR "the player's machine was not told the level (${results}):\n${said}")
endif()

execute_process(
    COMMAND ${lute} "${BACKEND}" "--port=${BACKEND_PORT}" --idle-exit=2
    COMMAND ${lute} "${TWICE}" "--port=${BACKEND_PORT}"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 60
    RESULT_VARIABLE twiceResults
    OUTPUT_VARIABLE twiceOutput
    ERROR_VARIABLE twiceOutput)
if(NOT twiceOutput MATCHES "\\[twice\\] once:")
    message(FATAL_ERROR "the same transaction sent twice was not done once (${twiceResults}):\n${twiceOutput}")
endif()
message("a game's own backend keeps its records, trades and shared values, and does a request once")
