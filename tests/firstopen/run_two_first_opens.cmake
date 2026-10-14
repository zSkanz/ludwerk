# Two first opens at once. A project's content is compiled the first
# time the project is opened, by whichever process opens it. Two that opened a
# never-opened project in the same moment each compiled it into the same
# store, and one of them read what the other was half way through writing: it
# ran with a model that had no clips. One run in twelve, on whichever side
# lost.
#
# Here it is done on purpose, several times over: a fresh copy of a project
# with one model, two hosts started on it together, and each asked whether
# the model's clips are there. Each writes its own log, because a driver that
# starts two processes hears only the last of them.
#
# What holds it now: one process compiles at a time (`platform::FileLock`,
# taken in `compileImported`), and the store's files are written beside their
# names and renamed over them.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/firstopen> -DCONTENT=<a project's content/>
#         -DSTAGE=<dir> -P run_two_first_opens.cmake

foreach(required HOST PROJECT CONTENT STAGE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_two_first_opens.cmake: -D${required}=... is required")
    endif()
endforeach()
if(NOT DEFINED ROUNDS)
    set(ROUNDS 6)
endif()

file(REMOVE_RECURSE "${STAGE}")
set(failures "")
set(waited 0)
foreach(round RANGE 1 ${ROUNDS})
    # A copy nobody has opened: no `.engine` folder, so nothing compiled.
    set(game "${STAGE}/round-${round}")
    file(MAKE_DIRECTORY "${game}")
    file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE PATTERN "*.cmake" EXCLUDE)
    file(COPY "${CONTENT}/" DESTINATION "${game}/content")

    execute_process(
        COMMAND "${HOST}" --headless --rhi=null --frames=150 --exit "--log-file=${game}/first.log" "${game}"
        COMMAND "${HOST}" --headless --rhi=null --frames=150 --exit "--log-file=${game}/second.log" "${game}"
        WORKING_DIRECTORY "${game}"
        RESULTS_VARIABLE results
        OUTPUT_QUIET
        ERROR_QUIET)
    foreach(result IN LISTS results)
        if(NOT result EQUAL 0)
            string(APPEND failures "  round ${round}: a host exited ${result}\n")
        endif()
    endforeach()

    foreach(which first second)
        set(said "")
        if(EXISTS "${game}/${which}.log")
            file(READ "${game}/${which}.log" said)
        endif()
        if(said MATCHES "is compiling this project's content")
            math(EXPR waited "${waited} + 1")
        endif()
        if(NOT said MATCHES "\\[firstopen\\] clips jump=[0-9.]+ slash=[0-9.]+")
            string(REGEX MATCH "\\[firstopen\\][^\n]*" line "${said}")
            string(APPEND failures "  round ${round}, the ${which} host: ${line}\n")
            if(line STREQUAL "")
                string(APPEND failures "    (it said nothing of its clips; its log is ${game}/${which}.log)\n")
            endif()
        endif()
        if(said MATCHES "\\[error\\]|\\[warn\\]")
            string(REGEX MATCH "\\[(error|warn)\\][^\n]*" line "${said}")
            string(APPEND failures "  round ${round}, the ${which} host warned or erred: ${line}\n")
        endif()
    endforeach()
endforeach()

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "two first opens: a host that opened the project beside another ran without its content:\n${failures}")
endif()
message("two first opens: ${ROUNDS} rounds, every host of every pair had the model's clips (a host waited for the other in ${waited} of them)")
