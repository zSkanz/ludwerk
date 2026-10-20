# **The ports the tests take, a block for each checkout on the machine.**
#
# The network tests bind ports written in them, all in 47000-47999: a world
# manager's front door and its worlds, a relay, a health and an admin server,
# the hosts of a match. Two checkouts running the same suite at once on one
# machine took each other's ports and failed for it -- a case of one failed,
# unnamed, while the other checkout ran the same suite. A port of the system's
# choosing (bind 0) would do for a process that is both ends, but the
# transport's seam has no such thing -- a zero port is a client -- and a
# manager hands its worlds a range before they start.
#
# So every port a test writes is moved down by the same thousand for the
# whole checkout: block 0 is 47000-47999 as written, block 1 46000-46999, and
# so on to block 5, 42000-42999. The C++ tests read `ENG_TEST_PORT_SHIFT`
# (`tests/support/engine_test_ports.h`); a test here names a port as
# `${ENG_PORT_47812}`.
#
# **The block is the machine's to give**: a list beside the brand's other
# per-machine files names each checkout's block, and a checkout not on it is
# given the lowest block nobody holds -- a hash of the path gave the two
# checkouts of one machine the same block of six. A checkout whose folder is
# gone gives its block back. `-DENG_TEST_PORT_BLOCK=<0-5>` chooses one. A
# container has a network of its own and nothing beside it, so it is block 0
# wherever its list is empty.
#
# **Nor a block the system keeps**: on Windows, Hyper-V, WSL and Docker hold
# ranges of ports nothing else may bind, and the ranges move from one boot to
# the next. A block that meets one is passed over, and a checkout whose block
# has come to meet one is given another, each said in the configure's output;
# a block that came to meet one after the configure needs the configure run
# again.

set(ENG_TEST_PORT_BLOCK "" CACHE STRING "The block of test ports this checkout uses, 0-5; empty asks the machine")

# The blocks that meet a range of ports the system keeps for itself.
function(engine_test_port_reserved out)
    set(blocks "")
    if(CMAKE_HOST_WIN32)
        foreach(protocol tcp udp)
            execute_process(COMMAND netsh interface ipv4 show excludedportrange protocol=${protocol}
                OUTPUT_VARIABLE table ERROR_QUIET RESULT_VARIABLE result)
            if(NOT result EQUAL 0)
                continue()
            endif()
            # A range is a line of two numbers, whatever language the table
            # is in.
            string(REGEX MATCHALL "[0-9]+[ \t]+[0-9]+" ranges "${table}")
            foreach(range IN LISTS ranges)
                string(REGEX MATCH "^([0-9]+)[ \t]+([0-9]+)$" whole "${range}")
                set(first ${CMAKE_MATCH_1})
                set(last ${CMAKE_MATCH_2})
                foreach(block RANGE 0 5)
                    math(EXPR low "47000 - ${block} * 1000")
                    math(EXPR high "${low} + 999")
                    if(NOT first GREATER high AND NOT last LESS low AND NOT block IN_LIST blocks)
                        list(APPEND blocks ${block})
                        message(STATUS "Test ports: block ${block} (${low}-${high}) meets ${protocol} ${first}-${last}, "
                                       "which the system keeps; it is passed over")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endif()
    set(${out} "${blocks}" PARENT_SCOPE)
endfunction()

function(engine_test_port_block out)
    if(NOT ENG_TEST_PORT_BLOCK STREQUAL "")
        if(NOT ENG_TEST_PORT_BLOCK MATCHES "^[0-5]$")
            message(FATAL_ERROR "ENG_TEST_PORT_BLOCK is ${ENG_TEST_PORT_BLOCK}: a block is 0 to 5")
        endif()
        set(${out} ${ENG_TEST_PORT_BLOCK} PARENT_SCOPE)
        return()
    endif()
    if(DEFINED ENV{LOCALAPPDATA})
        set(folder "$ENV{LOCALAPPDATA}/${ENG_BRAND_NAME}")
    elseif(DEFINED ENV{HOME})
        set(folder "$ENV{HOME}/.local/state/${ENG_BRAND_NAME}")
    else()
        set(${out} 0 PARENT_SCOPE)
        return()
    endif()
    file(TO_CMAKE_PATH "${folder}" folder)
    file(MAKE_DIRECTORY "${folder}")
    set(registry "${folder}/test-port-blocks.txt")
    # Two checkouts configured at once take turns.
    file(LOCK "${registry}.lock" GUARD FUNCTION TIMEOUT 60)
    engine_test_port_reserved(reserved)

    set(kept "")
    set(held "")
    set(mine "")
    if(EXISTS "${registry}")
        file(STRINGS "${registry}" lines)
        foreach(line IN LISTS lines)
            if(NOT line MATCHES "^([0-5]) (.+)$")
                continue()
            endif()
            set(block "${CMAKE_MATCH_1}")
            set(checkout "${CMAKE_MATCH_2}")
            if(checkout STREQUAL CMAKE_SOURCE_DIR)
                if(block IN_LIST reserved)
                    message(STATUS "Test ports: this checkout's block ${block} is the system's now; another is given")
                    continue()
                endif()
                set(mine ${block})
            elseif(NOT IS_DIRECTORY "${checkout}")
                continue()
            endif()
            list(APPEND kept "${line}")
            list(APPEND held ${block})
        endforeach()
    endif()
    if(mine STREQUAL "")
        foreach(block RANGE 0 5)
            if(NOT block IN_LIST held AND NOT block IN_LIST reserved)
                set(mine ${block})
                break()
            endif()
        endforeach()
        if(mine STREQUAL "")
            message(WARNING "every block of test ports is held or the system's (${registry}); "
                            "this checkout shares block 0")
            set(mine 0)
        else()
            list(APPEND kept "${mine} ${CMAKE_SOURCE_DIR}")
        endif()
    endif()
    list(JOIN kept "\n" text)
    file(WRITE "${registry}" "${text}\n")
    set(${out} ${mine} PARENT_SCOPE)
endfunction()

if(ENG_BUILD_TESTS)
    engine_test_port_block(engineTestPortBlock)
    math(EXPR ENG_TEST_PORT_SHIFT "${engineTestPortBlock} * 1000")
    math(EXPR engineTestPortLow "47000 - ${ENG_TEST_PORT_SHIFT}")
    math(EXPR engineTestPortHigh "47999 - ${ENG_TEST_PORT_SHIFT}")
    message(STATUS "Test ports: block ${engineTestPortBlock}, ${engineTestPortLow}-${engineTestPortHigh}")
    foreach(written RANGE 47000 47999)
        math(EXPR ENG_PORT_${written} "${written} - ${ENG_TEST_PORT_SHIFT}")
    endforeach()
else()
    set(ENG_TEST_PORT_SHIFT 0)
endif()
