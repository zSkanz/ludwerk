# A record carried by travel (ADR 0238 §2): under a manager, the town loads a
# player's record, changes it and sends the player to the fields in the same
# tick; the fields load it as the town left it. The town writes it as the
# player goes, and the fields wait for that write before they are given it.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_travel.cmake needs -D${required}=")
    endif()
endforeach()

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
set(game "${WORKDIR}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)
math(EXPR first "${PORT} + 1")
math(EXPR last "${PORT} + 2")
file(WRITE "${WORKDIR}/servers.toml"
     "[manager]\nport = ${PORT}\nworld_ports = \"${first}-${last}\"\n\n"
     "[[world]]\nname = \"town\"\nscene = \"scenes/town.scene.json\"\nentry = true\n\n"
     "[[world]]\nname = \"fields\"\nscene = \"scenes/fields.scene.json\"\n")

# The manager, its output kept apart, ends at the fields' line -- or the
# town's, a minute on, when nobody came; the player's machine ends at its own.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -DOUT=manager.out -P "${CMAKE_CURRENT_LIST_DIR}/../netcode/apart.cmake" --
            "${HOST}" "--manager=${WORKDIR}/servers.toml" "--exit-on-line=[travel] done"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=4800
            "--exit-on-line=[travel] client level=" "${game}"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 150
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
if(NOT town MATCHES "\\[travel\\] town set level=7")
    message(FATAL_ERROR "the town did not change the record and send the player on (${results}):\n${said}")
endif()
if(NOT fields MATCHES "\\[travel\\] done: fields level=7 from=town")
    message(FATAL_ERROR "the fields did not load the record as the town left it (${results}):\n${said}")
endif()
if(NOT output MATCHES "\\[travel\\] client level=7")
    message(FATAL_ERROR "the player's machine was not told the level (${results}):\n${said}")
endif()
message("the record changed in the tick of the hop is the one the next world loads")
