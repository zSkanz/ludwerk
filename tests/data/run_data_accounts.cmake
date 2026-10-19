# Accounts at the front door (ADR 0237 and its amendment): under a manager
# with `[accounts]`, a player's machine makes an account, meets each refusal a
# sign-in can, signs in and joins with its ticket. The town knows them by the
# account with no `OnJoinRequest` of the game's, changes their record and
# sends them to the fields, which know them by the same account and load that
# record. The password is in no log and not in the database.
cmake_minimum_required(VERSION 3.24)

foreach(required HOST PROJECT WORKDIR PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_data_accounts.cmake needs -D${required}=")
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
     "[accounts]\nenabled = true\n\n"
     "[[world]]\nname = \"town\"\nscene = \"scenes/town.scene.json\"\nentry = true\n\n"
     "[[world]]\nname = \"fields\"\nscene = \"scenes/fields.scene.json\"\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -DOUT=manager.out -P "${CMAKE_CURRENT_LIST_DIR}/../netcode/apart.cmake" --
            "${HOST}" "--manager=${WORKDIR}/servers.toml" "--exit-on-line=[accounts] done"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 --frames=7200
            "--scene-data={\"door\":\"127.0.0.1:${PORT}\"}" "--exit-on-line=[accounts] client account=" "${game}"
    WORKING_DIRECTORY "${WORKDIR}"
    TIMEOUT 200
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

set(town "")
set(fields "")
set(manager "")
if(EXISTS "${WORKDIR}/logs/town/town.log")
    file(READ "${WORKDIR}/logs/town/town.log" town)
endif()
if(EXISTS "${WORKDIR}/logs/fields/fields.log")
    file(READ "${WORKDIR}/logs/fields/fields.log" fields)
endif()
foreach(log "${WORKDIR}/manager.out" "${WORKDIR}/manager.out.err" "${WORKDIR}/logs/manager.log")
    if(EXISTS "${log}")
        file(READ "${log}" said)
        string(APPEND manager "${said}")
    endif()
endforeach()
set(all "--- the player's machine:\n${output}\n--- the manager:\n${manager}\n--- the town:\n${town}\n--- the fields:\n${fields}")

foreach(expected "registered ok=true" "again reason=Enum.SignInFailure.Taken"
                 "wrong reason=Enum.SignInFailure.WrongNameOrPassword"
                 "nobody reason=Enum.SignInFailure.WrongNameOrPassword"
                 "short reason=Enum.SignInFailure.InvalidPassword" "signed in ok=true")
    string(FIND "${output}" "[accounts] ${expected}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "the player's machine did not say `${expected}` (${results}):\n${all}")
    endif()
endforeach()
if(NOT town MATCHES "\\[accounts\\] town account=([0-9a-f]+) level=7")
    message(FATAL_ERROR "the town did not know the player by an account (${results}):\n${all}")
endif()
set(account "${CMAKE_MATCH_1}")
string(LENGTH "${account}" length)
if(NOT length EQUAL 32)
    message(FATAL_ERROR "the account id is not 128 bits: ${account}\n${all}")
endif()
if(NOT fields MATCHES "\\[accounts\\] done: fields account=${account} level=7 from=town")
    message(FATAL_ERROR "the fields did not know the traveller by the same account, with the town's record:\n${all}")
endif()
if(NOT output MATCHES "\\[accounts\\] client account=${account} level=7")
    message(FATAL_ERROR "the player's machine was not told its account and level:\n${all}")
endif()

# **The password is nowhere it should not be**: every log of every process,
# and the database's file and its journal, whose bytes are read as text.
string(FIND "${all}" "correct horse battery" leaked)
if(NOT leaked EQUAL -1)
    message(FATAL_ERROR "the password is in a log:\n${all}")
endif()
# Every other log under the run: each world's own, and the player's machine's.
file(GLOB_RECURSE logs "${WORKDIR}/*.log" "${WORKDIR}/*.out" "${WORKDIR}/*.err")
foreach(log ${logs})
    file(STRINGS "${log}" found REGEX "correct horse")
    if(found)
        message(FATAL_ERROR "the password is in ${log}")
    endif()
endforeach()
file(GLOB databases "${WORKDIR}/data/*")
foreach(database ${databases})
    file(STRINGS "${database}" found REGEX "correct horse")
    if(found)
        message(FATAL_ERROR "the password is in ${database}")
    endif()
endforeach()
message("an account made, signed in, and known by both worlds; the password in no log and no file")
