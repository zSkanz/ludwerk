# The notices a host's build wrote beside it (`cmake/engine_notices.cmake`):
# what it links is there, with a license text each, and what it does not is
# not.
#
#   cmake -DNOTICES=<content/licenses> -P check_notices.cmake
if(NOT IS_DIRECTORY "${NOTICES}")
    message(FATAL_ERROR "no notices were written: ${NOTICES}")
endif()
file(READ "${NOTICES}/THIRD_PARTY_NOTICES.md" list)

# Every host links these, and carries the shaders and fonts of the last four.
foreach(name IN ITEMS luau sdl3 jolt miniaudio stb fastgltf smaa inter)
    if(NOT list MATCHES "\n\\| ${name} \\|")
        message(FATAL_ERROR "${name} is not in the notices, and every host is built from it:\n${list}")
    endif()
endforeach()
# No host is built from the test framework.
if(list MATCHES "\n\\| doctest \\|")
    message(FATAL_ERROR "doctest is in a host's notices, and only the tests link it:\n${list}")
endif()

# A license text for every row that has one to carry.
string(REGEX MATCHALL "\n\\| [A-Za-z0-9_.+-]+ \\| [^\n]*" rows "${list}")
list(LENGTH rows count)
if(count LESS 8)
    message(FATAL_ERROR "the notices name ${count} projects:\n${list}")
endif()
foreach(row IN LISTS rows)
    string(REGEX REPLACE "^\n\\| ([A-Za-z0-9_.+-]+) \\| [^|]*\\| ([^|]*) \\|.*$" "\\1;\\2" parts "${row}")
    list(GET parts 0 name)
    list(GET parts 1 license)
    if(name STREQUAL "Project" OR license MATCHES "^Public Domain *$")
        continue()
    endif()
    file(GLOB texts "${NOTICES}/${name}/*")
    if(NOT texts)
        message(FATAL_ERROR "${name} is in the notices with no license text beside it")
    endif()
endforeach()
message(STATUS "notices: ${count} projects, each with its license")
