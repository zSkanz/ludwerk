# An answer from the compiler's cache is the answer a compile gives, and a
# model compiled on a worker is the model compiled alone.
#
# An export reads the editor's import cache (`assetc --cache`), so what the
# editor compiled is not compiled again. That is only right if a source
# answered from the cache comes out as the same bytes a compile of it makes.
# And models are compiled side by side, one to a worker, and merged in source
# order. Four processes over one tree of models: one with no cache, one with
# one worker (`--jobs 1`), one that fills an empty cache, and one answered from
# it -- which must compile nothing -- and every file the four write is
# compared. (`asset_determinism` holds the pictures and the chunks; its tree
# has no model.)
cmake_minimum_required(VERSION 3.24)

foreach(required ASSETC CONTENT WORKDIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_cache_gate.cmake needs -D${required}=")
    endif()
endforeach()

function(compile_into label)
    set(extra "")
    if(ARGC GREATER 1 AND "${ARGV1}" STREQUAL "serial")
        set(extra --jobs 1)
    elseif(ARGC GREATER 1)
        set(extra --cache "${WORKDIR}/cache")
    endif()
    set(out "${WORKDIR}/${label}")
    file(REMOVE_RECURSE "${out}")
    file(MAKE_DIRECTORY "${out}")
    execute_process(
        COMMAND "${ASSETC}"
            --input "${CONTENT}"
            --output "${out}/content.lpack"
            --manifest "${out}/content.manifest.json"
            ${extra}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "assetc failed for ${label} (${result}):\n${output}")
    endif()
    set(said "${output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORKDIR}/cache")
compile_into(plain)
compile_into(serial serial)
compile_into(cold cache)
# A tree with nothing the cache keeps would hold this gate to nothing.
if(NOT said MATCHES "assetc: cache: 0 answered, [1-9][0-9]* compiled")
    message(FATAL_ERROR "a compile into an empty cache did not compile into it:\n${said}")
endif()
compile_into(answered cache)
if(NOT said MATCHES "assetc: cache: [1-9][0-9]* answered, 0 compiled")
    message(FATAL_ERROR "a second compile through the same cache compiled again:\n${said}")
endif()

file(GLOB_RECURSE plainFiles RELATIVE "${WORKDIR}/plain" "${WORKDIR}/plain/*")
file(GLOB_RECURSE serialFiles RELATIVE "${WORKDIR}/serial" "${WORKDIR}/serial/*")
file(GLOB_RECURSE coldFiles RELATIVE "${WORKDIR}/cold" "${WORKDIR}/cold/*")
file(GLOB_RECURSE answeredFiles RELATIVE "${WORKDIR}/answered" "${WORKDIR}/answered/*")
set(allFiles ${plainFiles} ${serialFiles} ${coldFiles} ${answeredFiles})
list(REMOVE_DUPLICATES allFiles)
list(SORT allFiles)

set(differences "")
foreach(relative IN LISTS allFiles)
    set(a "${WORKDIR}/plain/${relative}")
    set(b "${WORKDIR}/cold/${relative}")
    set(c "${WORKDIR}/answered/${relative}")
    set(d "${WORKDIR}/serial/${relative}")
    if(NOT EXISTS "${a}" OR NOT EXISTS "${b}" OR NOT EXISTS "${c}" OR NOT EXISTS "${d}")
        list(APPEND differences "${relative}: written by only some of the four compiles")
        continue()
    endif()
    file(SHA256 "${a}" hashA)
    file(SHA256 "${b}" hashB)
    file(SHA256 "${c}" hashC)
    file(SHA256 "${d}" hashD)
    if(NOT hashA STREQUAL hashD)
        list(APPEND differences "${relative}: side by side and --jobs 1 differ -- ${hashA} != ${hashD}")
    elseif(NOT hashA STREQUAL hashB OR NOT hashA STREQUAL hashC)
        list(APPEND differences "${relative}: ${hashA} with no cache, ${hashB} filling it, ${hashC} answered")
    endif()
endforeach()

if(NOT differences STREQUAL "")
    string(REPLACE ";" "\n  " report "${differences}")
    message(FATAL_ERROR "the compiles of one tree differ:\n  ${report}")
endif()
list(LENGTH allFiles compared)
message(STATUS "asset cache: ${compared} file(s) the same side by side, with one worker, filling a cache "
               "and answered from it")
