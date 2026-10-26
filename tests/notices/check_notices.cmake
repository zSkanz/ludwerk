# The notices a host's build wrote beside it (`cmake/engine_notices.cmake`):
# what it links is there, with a license text each, and what it does not is
# not.
#
#   cmake -DNOTICES=<content/licenses> -P check_notices.cmake
if(NOT IS_DIRECTORY "${NOTICES}")
    message(FATAL_ERROR "no notices were written: ${NOTICES}")
endif()
file(READ "${NOTICES}/THIRD_PARTY_NOTICES.md" whole)
# The projects' table, and after it the table of the files carried.
string(FIND "${whole}" "\n## Files carried" split)
if(split EQUAL -1)
    message(FATAL_ERROR "the notices have no table of the files carried:\n${whole}")
endif()
string(SUBSTRING "${whole}" 0 ${split} list)
string(SUBSTRING "${whole}" ${split} -1 carriedTable)

# Every host links these, and carries the shaders and fonts of the last three.
foreach(name IN ITEMS luau sdl3 jolt miniaudio stb fastgltf smaa inter cousine)
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

# **Every font the host carries is credited**, with a license text: its file
# named in the table of the files carried, against a project that has a row
# and a text. A font copied beside the binary is no part of the link graph,
# and one was shipped with no notice at all.
if(DEFINED CONTENT)
    file(GLOB_RECURSE fonts LIST_DIRECTORIES false RELATIVE "${CONTENT}" "${CONTENT}/fonts/*")
    if(NOT fonts)
        message(FATAL_ERROR "the host carries no fonts under ${CONTENT}/fonts, and every host carries two")
    endif()
    foreach(font IN LISTS fonts)
        string(REGEX MATCH "\n\\| ${font} \\| ([A-Za-z0-9_.+-]+) \\|" carried "${carriedTable}")
        if(carried STREQUAL "")
            message(FATAL_ERROR "the host carries ${font}, and the notices do not say whose it is:\n${whole}")
        endif()
        set(project "${CMAKE_MATCH_1}")
        file(GLOB texts "${NOTICES}/${project}/*")
        if(NOT list MATCHES "\n\\| ${project} \\|" OR NOT texts)
            message(FATAL_ERROR "the host carries ${font} as ${project}'s, and ${project} has no row or license text")
        endif()
    endforeach()
    list(LENGTH fonts fontCount)
    message(STATUS "notices: ${fontCount} fonts, each credited")
endif()
message(STATUS "notices: ${count} projects, each with its license")
