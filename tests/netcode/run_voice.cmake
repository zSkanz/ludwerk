# Two players talking over a link, clean and then lossy (ADR 0256).
#
# A real server and two real joiners over the real transport. Each joiner
# speaks a recorded phrase of its own in place of a microphone (`--voice-in`),
# played at real time and again from its start, and writes the voices it
# heard to a file (`--voice-out`). The run is made twice: once over a clean
# link, and once with the second joiner's link losing a tenth of what it
# sends and receives, both ways, and jittered. Held, of each joiner, each run:
#
#   - the other's voice arrives and plays: most of the frames the run had
#     time for, not a few;
#   - little of it is concealed: a tenth lost is about a tenth, give or take
#     the jitter buffer running dry, and not a voice that is half gaps;
#   - it is at the level it was said: the codec, the concealment and the mix
#     neither lose it nor double it;
#   - **it is the speech that was said** (`voice_match`): what was heard,
#     lined up against the other joiner's phrase, has its spectral envelope,
#     and not the joiner's own phrase's -- a voice from the wrong speaker,
#     one heard twice over itself, or noise fails.
#
# And it says, for the ADR, how late a voice is heard: from the speaker's
# microphone to the listener's ear, the capture, the codec, the link and the
# jitter buffer.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DMATCH=<voice_match> -DPROJECT=<tests/netcode/voice>
#         -DFIRST_PHRASE=<wav> -DSECOND_PHRASE=<wav> -DSTAGE=<dir> -DPORT=<port>
#         -P run_voice.cmake

foreach(required HOST MATCH PROJECT FIRST_PHRASE SECOND_PHRASE STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_voice.cmake: -D${required}=... is required")
    endif()
endforeach()

set(failures "")

# One run: `link` is what the second joiner's link adds.
function(voice_run name link)
    set(stage "${STAGE}/${name}")
    file(REMOVE_RECURSE "${stage}")
    file(COPY "${PROJECT}/" DESTINATION "${stage}" PATTERN ".engine" EXCLUDE)
    # Twelve seconds of server, ten of each joiner: about nine of talking
    # once both have joined.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DOUT=server.out -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apart.cmake" --
                "${HOST}" --headless --pace=60 "--serve=${PORT}" --frames=720 "${stage}"
        COMMAND "${CMAKE_COMMAND}" -DOUT=first.out -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/apart.cmake" --
                "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=600 "--net-delay=25"
                "--voice-in=${FIRST_PHRASE}" "--voice-out=${stage}/first-heard.wav" "${stage}"
        COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=600 "--net-delay=25"
                ${link} "--voice-in=${SECOND_PHRASE}" "--voice-out=${stage}/second-heard.wav" "${stage}"
        WORKING_DIRECTORY "${stage}"
        RESULT_VARIABLE results
        OUTPUT_VARIABLE second
        ERROR_VARIABLE second)
    file(READ "${stage}/first.out" first)

    # Who said what, from when: each joiner's phrase and its microphone's
    # start, and the other's.
    foreach(machine first second)
        string(REGEX MATCH "The microphone is a file for this run, from ([0-9]+) ns" _ "${${machine}}")
        set(${machine}Spoke "${CMAKE_MATCH_1}")
    endforeach()
    set(firstSaid "${SECOND_PHRASE}")
    set(firstOwn "${FIRST_PHRASE}")
    set(firstFrom "${secondSpoke}")
    set(secondSaid "${FIRST_PHRASE}")
    set(secondOwn "${SECOND_PHRASE}")
    set(secondFrom "${firstSpoke}")

    foreach(machine first second)
        string(REGEX MATCH
               "from ([0-9]+) ns on the steady clock: ([0-9]+) frames played, ([0-9]+) of them concealed, at (-?[0-9]+) dBFS"
               line "${${machine}}")
        if(line STREQUAL "" OR "${${machine}From}" STREQUAL "")
            message("${name}, ${machine}:\n${${machine}}")
            string(APPEND failures "  ${name}: the ${machine} joiner wrote no voices heard, or the other said nothing\n")
            continue()
        endif()
        set(recorded ${CMAKE_MATCH_1})
        set(played ${CMAKE_MATCH_2})
        set(concealed ${CMAKE_MATCH_3})
        set(level ${CMAKE_MATCH_4})
        execute_process(
            COMMAND "${MATCH}" "${stage}/${machine}-heard.wav" "${${machine}Said}" "${${machine}Own}" "${recorded}"
                    "${${machine}From}"
            OUTPUT_VARIABLE matched
            RESULT_VARIABLE matchResult)
        string(REGEX MATCH
               "similarity=(-?[0-9.]+) other=(-?[0-9.]+) margin=(-?[0-9.]+) voiced=([0-9.]+) latency_ms=([0-9]+)" _
               "${matched}")
        set(similarity ${CMAKE_MATCH_1})
        set(other ${CMAKE_MATCH_2})
        set(margin ${CMAKE_MATCH_3})
        set(latency ${CMAKE_MATCH_5})
        message("${name}, ${machine} joiner: ${played} frames played, ${concealed} concealed, ${level} dBFS, "
                "similarity ${similarity} to what was said against ${other} to its own phrase, "
                "mouth to ear ${latency} ms")
        if(NOT matchResult EQUAL 0 OR similarity STREQUAL "")
            string(APPEND failures "  ${name}: voice_match could not compare the ${machine} joiner's file: ${matched}\n")
            continue()
        endif()
        # About nine seconds of talking is 450 frames; a joiner that heard
        # less than half of that heard a few words.
        if(played LESS 225)
            string(APPEND failures "  ${name}: the ${machine} joiner played ${played} frames, under 225\n")
        endif()
        math(EXPR ceiling "${played} * ${CONCEALED_CEILING} / 100")
        if(concealed GREATER ceiling)
            string(APPEND failures
                   "  ${name}: the ${machine} joiner concealed ${concealed} of ${played} frames, over ${CONCEALED_CEILING}%\n")
        endif()
        if(level LESS -24 OR level GREATER -12)
            string(APPEND failures "  ${name}: the ${machine} joiner heard ${level} dBFS, not within 6 dB of -18\n")
        endif()
        if(similarity LESS ${SIMILAR_FLOOR})
            string(APPEND failures
                   "  ${name}: the ${machine} joiner heard something of similarity ${similarity}, under ${SIMILAR_FLOOR}\n")
        endif()
        # Its own phrase is a voice too, so it correlates some: what was said
        # must correlate clearly more.
        if(margin LESS 0.4)
            string(APPEND failures "  ${name}: the ${machine} joiner's file is as like its own phrase (${other}) "
                                   "as the other's (${similarity})\n")
        endif()
        # A frame captured, the link's 50 ms of round trip, and a buffer two
        # to six frames deep: 110 to 130 ms here. Four times that is a
        # buffer that never shrank or a voice queued behind the world.
        if(latency GREATER 500)
            string(APPEND failures "  ${name}: the ${machine} joiner heard the voice ${latency} ms late, over 500\n")
        endif()
    endforeach()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Measured where this was written: clean, nothing concealed and a similarity
# of 0.93 and 0.96 against 0.20 and 0.24 to the joiner's own phrase; a tenth
# lost, 9 and 11% concealed, 0.91 against 0.29 and 0.25; heard 109 to 127 ms
# after it was said.
set(CONCEALED_CEILING 10)
set(SIMILAR_FLOOR 0.85)
voice_run(clean "")
set(CONCEALED_CEILING 25)
set(SIMILAR_FLOOR 0.80)
voice_run(lossy "--net-jitter=10;--net-loss=10")

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "voice between two players:\n${failures}")
endif()
