# A mesh a script names is measured by the machine that owns the part
# (ADR 0258), and the other machines take what it measured.
#
# A real server and a real joiner, two processes over the real transport. The
# server makes `MeshPart`s by script: one naming the pane in its content and
# nothing else, one naming it after its `Size` was set, and one naming a mesh
# the content does not hold. The joiner (`tests/netcode/measured/src/client/
# Seen.luau`) reads each against what the server holds -- `MeshSize`, `Size`,
# and what a ray finds just inside and just outside the part's half-width on
# its own physics -- then makes a part of its own, and names the pane on a
# part of the server's that named none.
#
# Held: the pane's measure on both ends, the collider the part's `Size` on
# both, a `Size` that was set kept, a mesh nobody holds left at (1, 1, 1) and
# said once, the joiner's own part measured there, and the server's part
# NOT measured on the joiner -- a copy takes the server's numbers.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/measured> -DSTAGE=<dir>
#         -DPORT=<port> -P run_measured.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_measured.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)

# Compiled once before the two open it, so neither waits on the other's
# import: what is measured is what the content holds, however it got there.
execute_process(
    COMMAND "${HOST}" --headless --rhi=null --frames=2 --exit "${game}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "measured: the project did not open (${result})")
endif()

# Neither draws (`--rhi=null`): the measure comes from the content, not from
# a mesh anybody loaded.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -DOUT=server.out -P "${CMAKE_CURRENT_LIST_DIR}/apart.cmake" -- "${HOST}" --headless --rhi=null --pace=60 "--serve=${PORT}" --frames=3600 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=3000 "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
file(READ "${game}/server.out" served)
file(READ "${game}/server.out.err" servedErrors)
string(APPEND served "${servedErrors}")

# The pane: a cube scaled to 2.8 by 2 by 0.08 in its file.
set(pane "2.8000,2.0000,0.0800")
set(one "1.0000,1.0000,1.0000")
set(failures "")
function(expect name text)
    string(REGEX MATCH "\\[measured\\] ${name} [^\n]*" line "${output}")
    string(FIND "${line}" "${text}" at)
    if(line STREQUAL "")
        set(failures "${failures}  ${name}: the joiner said nothing of it\n" PARENT_SCOPE)
    elseif(at EQUAL -1)
        set(failures "${failures}  ${name}: not `${text}` in: ${line}\n" PARENT_SCOPE)
    endif()
endfunction()

expect(Served "Served same MeshSize=${pane} Size=${pane} Inside=Served Outside=miss")
expect(Sized "Sized same MeshSize=${pane} Size=3.0000,3.0000,3.0000 Inside=Sized Outside=miss")
expect(Missing "Missing same MeshSize=${one} Size=${one} Inside=Missing Outside=miss")
expect(Mine "Mine MeshSize=${pane} Size=${pane} Inside=Mine Outside=miss")
expect(Bare "Bare MeshSize=${one} Size=${one}")
if(NOT output MATCHES "\\[measured\\] done")
    string(APPEND failures "  the joiner did not finish\n")
endif()

# The mesh nobody holds is said, once, by the machine that tried.
string(REGEX MATCHALL "models/none\\.gltf\" could not be read" said "${served}")
list(LENGTH said times)
if(NOT times EQUAL 1)
    string(APPEND failures "  the server said the missing mesh ${times} times, not once\n")
endif()

if(NOT failures STREQUAL "")
    message("${output}")
    message("--- server ---\n${served}")
    message(FATAL_ERROR "measured: what a script-made MeshPart measured --\n${failures}")
endif()
message("measured: the pane measured ${pane} on the server and taken as it is by the joiner")
