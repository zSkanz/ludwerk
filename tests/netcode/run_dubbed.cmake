# The two-process dubbing gate (ADR 0200, protocol 45).
#
# A server says a line; a joiner hears it. The two do not hear the same
# recording -- the server resolves in the project's default language, the
# joiner runs in Brazilian Portuguese -- and the gate holds what must not
# depend on that:
#
#   - the line is as long as its LONGEST language on both machines (nine
#     tenths of a second, where the server's own file is five), and lasts the
#     same number of ticks on both;
#   - the line arrives as a voice, with its caption, and the joiner reads it in
#     ITS text language;
#   - the joiner found its own language's recording (under `Auto` a caption is
#     not wanted for a line heard in the language being read), and a line with
#     no recording at all is silent, lasts its caption, and wants one.
#
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/dubbed> -DSTAGE=<dir>
#         -DPORT=<n> -P run_dubbed.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_dubbed.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE PATTERN "tools" EXCLUDE)

execute_process(
    COMMAND "${HOST}" --headless --pace=60 "--serve=${PORT}" --frames=3000 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=2400
            --locale=pt-BR --voice-locale=pt-BR "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output
    # The joiner says words with accents in them, and says them in UTF-8.
    ENCODING UTF8)

set(failures "")
function(line_of out what)
    string(REGEX MATCH "\\[dubbed\\] ${what}[^\n]*" found "${output}")
    set(${out} "${found}" PARENT_SCOPE)
    if(found STREQUAL "")
        set(failures "${failures}  the joiner said nothing of: ${what}\n" PARENT_SCOPE)
    endif()
endfunction()
function(holds line text)
    string(FIND "${line}" "${text}" at)
    if(at EQUAL -1)
        set(failures "${failures}  not `${text}` in: ${line}\n" PARENT_SCOPE)
    endif()
endfunction()
# The ticks a line lasted, as one machine counted them.
function(ticks_of out line)
    string(REGEX MATCH "ticks=([0-9]+)" _ "${line}")
    set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

line_of(joiner "joiner ")
holds("${joiner}" "text=pt-BR")
holds("${joiner}" "voice=pt-BR")
holds("${joiner}" "voices=en,pt-BR")

# The recorded line: a voice, nine tenths long on both ends, read in
# Portuguese, and heard in it -- so `Auto` wants no caption.
line_of(heardHello "heard act1.hello ")
holds("${heardHello}" "category=Voice")
holds("${heardHello}" "length=0.90")
holds("${heardHello}" "shows=false")
holds("${heardHello}" "speaker=Guia")
holds("${heardHello}" "text=Olá, e bem-vindo.")
line_of(serverHello "server hello ")
holds("${serverHello}" "category=Voice")
holds("${serverHello}" "length=0.90")

# The line nobody recorded: a second and a half and six hundredths for each of
# the thirty-five characters of the DEFAULT language's text, on both ends,
# whatever each reads -- and a caption is wanted, since nothing is heard.
line_of(heardLater "heard act1.later ")
holds("${heardLater}" "length=3.60")
holds("${heardLater}" "shows=true")
holds("${heardLater}" "text=Esta ainda não foi gravada.")
line_of(serverLater "server later ")
holds("${serverLater}" "length=3.60")

line_of(done "done")

# The same number of ticks on both machines, give or take the trip: a replica
# starts a line when it is told and ends it when it is told.
# No variable here is called what a line is called: an older CMake reads a
# quoted name in an `if` as the variable of that name.
foreach(pair "hello|54" "later|216")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 name)
    list(GET parts 1 expected)
    if(name STREQUAL "hello")
        ticks_of(heard "${heardHello}")
        ticks_of(said "${serverHello}")
    else()
        ticks_of(heard "${heardLater}")
        ticks_of(said "${serverLater}")
    endif()
    if(heard STREQUAL "" OR said STREQUAL "")
        string(APPEND failures "  no count of ticks for ${name}\n")
        continue()
    endif()
    math(EXPR low "${expected} - 1")
    math(EXPR high "${expected} + 2")
    if(said LESS low OR said GREATER high)
        string(APPEND failures "  the server's ${name} lasted ${said} ticks, not about ${expected}\n")
    endif()
    math(EXPR apart "${heard} - ${said}")
    if(apart LESS -8 OR apart GREATER 8)
        string(APPEND failures "  ${name} lasted ${said} ticks on the server and ${heard} on the joiner\n")
    endif()
endforeach()

message("${joiner}
${heardHello}
${serverHello}
${heardLater}
${serverLater}")

if(NOT failures STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "dubbed: a joiner did not hear the line the server said as it should:\n${failures}")
endif()
message("dubbed: a line the server said was heard in the joiner's language, and was as long on both")
