# **The notices a binary carries are of what it is built from** -- found, not
# listed.
#
# `THIRD_PARTY_NOTICES.md` at the root is the repository's whole list: the
# editor's ImGui, the tests' doctest, the importer's assimp, the tools an
# installation runs. A game exported with the engine carried that list and the
# license texts of the tools beside it, and not one text of what its player
# does link. So the build writes, beside each host it makes, the notices of
# that host alone: `engine_write_notices` walks the target's link graph --
# every target it links, through every target those link -- and takes each
# `third_party/<name>/` the graph's sources, include directories and link
# items name. What the graph cannot see, a project whose files are compiled
# into or copied as the engine's content (a shader's source, a font), is
# declared where it is used, with `engine_notice_uses`. A project found is
# credited only if `third_party/manifest.json` has a row for it, and one whose
# folder holds no license text stops the configure: a binary that ships it
# must carry its terms.
include_guard(GLOBAL)

# Projects whose files become content rather than code: credited in every
# host's notices, since every host carries the content.
function(engine_notice_uses)
    set_property(GLOBAL APPEND PROPERTY ENG_NOTICE_USES ${ARGN})
endfunction()

# Every `third_party/<name>` that `text` names.
function(_engine_notice_names text out)
    string(REGEX MATCHALL "third_party/[A-Za-z0-9_.+-]+" found "${text}")
    set(names "")
    foreach(match IN LISTS found)
        string(REGEX REPLACE "^third_party/" "" name "${match}")
        list(APPEND names "${name}")
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

function(_engine_notice_walk target)
    get_target_property(aliased ${target} ALIASED_TARGET)
    if(aliased)
        set(target ${aliased})
    endif()
    get_property(seen GLOBAL PROPERTY _ENG_NOTICE_SEEN)
    if(target IN_LIST seen)
        return()
    endif()
    set_property(GLOBAL APPEND PROPERTY _ENG_NOTICE_SEEN ${target})

    get_target_property(directory ${target} SOURCE_DIR)
    set(text "${directory}")
    # A source named relative to the directory that made the target -- the
    # projects `third_party/CMakeLists.txt` compiles itself name theirs so.
    get_target_property(sources ${target} SOURCES)
    if(sources)
        foreach(source IN LISTS sources)
            if(IS_ABSOLUTE "${source}")
                string(APPEND text ";${source}")
            else()
                string(APPEND text ";${directory}/${source}")
            endif()
        endforeach()
    endif()
    set(linked "")
    foreach(property INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SOURCES LINK_LIBRARIES
                     INTERFACE_LINK_LIBRARIES)
        get_target_property(value ${target} ${property})
        if(value)
            string(APPEND text ";${value}")
            if(property MATCHES "LINK_LIBRARIES$")
                list(APPEND linked ${value})
            endif()
        endif()
    endforeach()
    _engine_notice_names("${text}" names)
    set_property(GLOBAL APPEND PROPERTY _ENG_NOTICE_FOUND ${names})

    # Every word of a link item that is a target, generator expressions and
    # all: `$<LINK_ONLY:x>` and `$<$<PLATFORM_ID:Android>:x>` both link `x`
    # somewhere, and a notice too many is no harm where one too few is.
    foreach(item IN LISTS linked)
        string(REGEX MATCHALL "[A-Za-z0-9_.+:-]+" words "${item}")
        foreach(word IN LISTS words)
            if(TARGET "${word}")
                _engine_notice_walk("${word}")
            endif()
        endforeach()
    endforeach()
endfunction()

# Writes `out/THIRD_PARTY_NOTICES.md` -- the projects `target` is built from --
# and each one's license texts under `out/<name>/`. Call it deferred, once
# every target exists.
function(engine_write_notices target out)
    set_property(GLOBAL PROPERTY _ENG_NOTICE_SEEN "")
    set_property(GLOBAL PROPERTY _ENG_NOTICE_FOUND "")
    _engine_notice_walk(${target})
    get_property(found GLOBAL PROPERTY _ENG_NOTICE_FOUND)
    get_property(uses GLOBAL PROPERTY ENG_NOTICE_USES)
    list(APPEND found ${uses})
    list(REMOVE_DUPLICATES found)

    file(REMOVE_RECURSE "${out}")
    file(MAKE_DIRECTORY "${out}")
    file(READ "${CMAKE_SOURCE_DIR}/third_party/manifest.json" manifest)
    set(rows "")
    foreach(kind IN ITEMS dependencies archives)
        string(JSON count ERROR_VARIABLE missing LENGTH "${manifest}" ${kind})
        if(missing OR count EQUAL 0)
            continue()
        endif()
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON path GET "${manifest}" ${kind} ${index} path)
            get_filename_component(folder "${path}" NAME)
            if(NOT folder IN_LIST found OR NOT IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${path}")
                continue()
            endif()
            string(JSON name GET "${manifest}" ${kind} ${index} name)
            string(JSON version GET "${manifest}" ${kind} ${index} version)
            string(JSON license GET "${manifest}" ${kind} ${index} license)
            string(JSON upstream ERROR_VARIABLE noUpstream GET "${manifest}" ${kind} ${index} upstream)
            if(noUpstream)
                string(JSON upstream ERROR_VARIABLE noSource GET "${manifest}" ${kind} ${index} source)
            endif()
            file(GLOB texts LIST_DIRECTORIES false
                "${CMAKE_SOURCE_DIR}/${path}/[Ll][Ii][Cc][Ee][Nn][CcSs][Ee]*"
                "${CMAKE_SOURCE_DIR}/${path}/[Cc][Oo][Pp][Yy][Ii][Nn][Gg]*"
                "${CMAKE_SOURCE_DIR}/${path}/[Nn][Oo][Tt][Ii][Cc][Ee]*")
            if(texts)
                file(COPY ${texts} DESTINATION "${out}/${name}")
            elseif(NOT license MATCHES "^Public Domain$")
                message(FATAL_ERROR "${target} is built from ${name} (${license}), and ${path} holds no license "
                                    "text for it to carry: a LICENSE, COPYING or NOTICE file at its root.")
            endif()
            list(APPEND rows "| ${name} | ${version} | ${license} | ${upstream} |")
        endforeach()
    endforeach()
    list(SORT rows CASE INSENSITIVE)
    list(JOIN rows "\n" table)
    file(WRITE "${out}/THIRD_PARTY_NOTICES.md"
        "# Third-Party Notices\n\n"
        "The projects this program is built from, each under its own license. The texts are in the folder of\n"
        "each one's name beside this file. Written by the build, from what the program links and the files it\n"
        "carries.\n\n"
        "| Project | Version | License | Upstream |\n"
        "|---|---|---|---|\n"
        "${table}\n")
endfunction()
