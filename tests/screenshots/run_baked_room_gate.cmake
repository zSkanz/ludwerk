# A volume's light, kept in a file (ADR 0207), asked of the white furnace
# (`run_furnace_gate.cmake`): a closed grey room, one lamp, a volume of two
# bounces. The room is copied beside the build first -- a bake writes into the
# project it is asked of, and the tree is not somewhere to write.
#
#   the bake   `--bake-irradiance` works the volume out and writes the file
#              its `Baked` names, and says so.
#   kept       the room opened again, at a setting that works volumes out,
#              takes its light from the file and says no ray was cast -- and
#              **is the same picture as the room worked out by rays**.
#   flat       the room at a setting that works NO volume out is lit by one
#              colour, the mean of the file -- which the host says -- and is
#              the same picture as the room with no volume and an ambient of
#              that colour. With no file it is the room with no ambient at all.
#   refused    a volume of another spacing does not take the file's light, a
#              file that is not there is said to be missing, and a room with
#              a pillar that was not there is said -- once, where the tools
#              are -- not to be the room the file was baked in.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<the furnace>
#         -DOUTPUT=<dir> -P run_baked_room_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

# The host is read by the keys of what it says, not by the words (`--log-keys`).
set(ENV{ENG_LOG_KEYS} 1)
foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_baked_room_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

# A copy of its own, made afresh: nothing of the last run's file is there.
file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(COPY "${SCRIPT}/src" DESTINATION "${OUTPUT}/room")
set(ROOM "${OUTPUT}/room")
set(BAKED "asset://light/room.irradiance")

# As `run_split_screen_gate.cmake` has it, and for its reason.
function(device_was_removed out)
    set(${out} FALSE PARENT_SCOPE)
    if(NOT host_output MATCHES "0x887A0005|device instance has been suspended")
        return()
    endif()
    message("${host_output}")
    message("ENG_TEST_SKIP: the graphics device was removed under the test")
    set(${out} TRUE PARENT_SCOPE)
endfunction()

# One run of the host over the room. `said` is what it wrote.
function(run said)
    execute_process(
        COMMAND "${HOST}" "${ROOM}" --headless --locale=en --width=480 --height=270
                --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows --no-anti-aliasing ${ARGN}
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    set(${said} "${host_output}" PARENT_SCOPE)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0)
        device_was_removed(removed)
        if(removed)
            set(skipped TRUE PARENT_SCOPE)
            return()
        endif()
        message("${host_output}")
        message(FATAL_ERROR "baked room gate: the host exited ${host_result} (${ARGN})")
    endif()
endfunction()

function(picture name said gi data)
    file(REMOVE "${OUTPUT}/${name}.png")
    run(written --frames=40 --exit "--screenshot=${OUTPUT}/${name}.png" "--scene-data=${data}"
        "--global-illumination=${gi}")
    set(${said} "${written}" PARENT_SCOPE)
    set(skipped ${skipped} PARENT_SCOPE)
    if(NOT skipped AND NOT EXISTS "${OUTPUT}/${name}.png")
        message("${written}")
        message(FATAL_ERROR "baked room gate: no picture of `${name}` was written")
    endif()
endfunction()

function(ask name)
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${name}.png" ${ARGN}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${name}: ${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR "baked room gate: `${name}` is not as it should be (see above); it is in ${OUTPUT}")
    endif()
endfunction()

function(must_say said what why)
    if(NOT said MATCHES "${what}")
        message("${said}")
        message(FATAL_ERROR "baked room gate: ${why}")
    endif()
endfunction()
function(must_not_say said what why)
    if(said MATCHES "${what}")
        message("${said}")
        message(FATAL_ERROR "baked room gate: ${why}")
    endif()
endfunction()

# The wall ahead, to each side of it, the ceiling and the floor.
set(places "0.450,0.450:0.550,0.550==" "0.300,0.400:0.400,0.600==" "0.600,0.400:0.700,0.600=="
           "0.450,0.060:0.550,0.120==" "0.450,0.900:0.550,0.960==")
set(skipped FALSE)

# --- The room worked out by rays, with no file: what the file must be ----------
picture(bounced said high "{\"volume\":true}")
if(skipped)
    return()
endif()
# The run speaks by keys at all: without that, the line below holds nothing.
must_say("${said}" "\\[engine.frame.info.summary\\]" "the host did not say its messages by their keys")
must_not_say("${said}" "app.info.irradiance_kept" "a volume that names no file said it was lit by one")

# --- A file that is not there is said to be missing, and the room is worked out
picture(missing said high "{\"volume\":true,\"baked\":\"${BAKED}\"}")
if(skipped)
    return()
endif()
must_say("${said}" "app.warn.irradiance_baked_missing" "a volume whose file is not there did not say so")
ask(missing --tolerance=2 "--against=${OUTPUT}/bounced.png" ${places})

# --- The bake ------------------------------------------------------------------
run(said --bake-irradiance "--scene-data={\"volume\":true,\"baked\":\"${BAKED}\"}")
if(skipped)
    return()
endif()
must_say("${said}" "app.info.irradiance_baked\\][^\n]*${BAKED}: 512 point" "the bake did not say it wrote the room's 512 points")
if(NOT EXISTS "${ROOM}/content/light/room.irradiance")
    message("${said}")
    message(FATAL_ERROR "baked room gate: the bake wrote no file under the project's content")
endif()
# A project with no volume that names a file has nothing to bake, and says so.
execute_process(
    COMMAND "${HOST}" "${ROOM}" --bake-irradiance "--scene-data={\"volume\":true}"
    RESULT_VARIABLE none_result
    OUTPUT_VARIABLE none_output
    ERROR_VARIABLE none_output)
if(none_result EQUAL 0 OR NOT none_output MATCHES "app.err.irradiance_bake_none")
    message("${none_output}")
    message(FATAL_ERROR "baked room gate: a bake with nothing to bake exited ${none_result} and did not say so")
endif()

# --- Kept: the file's light, no ray cast, and the picture the rays made ---------
picture(kept said high "{\"volume\":true,\"baked\":\"${BAKED}\"}")
if(skipped)
    return()
endif()
must_say("${said}" "app.info.irradiance_kept\\][^\n]*${BAKED}: 512 point" "the room did not take its light from its file")
must_not_say("${said}" "\\[warn\\]" "a room lit by its own file warned of something")
ask(kept --tolerance=2 "--against=${OUTPUT}/bounced.png" ${places})

# --- Flat: a machine that works no volume out lights the room with the mean ------
picture(flat said low "{\"volume\":true,\"baked\":\"${BAKED}\"}")
if(skipped)
    return()
endif()
if(NOT said MATCHES "app.info.irradiance_flat\\][^\n]*${BAKED}: ([0-9.]+) ([0-9.]+) ([0-9.]+)")
    message("${said}")
    message(FATAL_ERROR "baked room gate: a machine that works no volume out did not say what colour the room is")
endif()
set(mean "${CMAKE_MATCH_1}")
message("the file's mean: ${CMAKE_MATCH_1} ${CMAKE_MATCH_2} ${CMAKE_MATCH_3}")
# A grey room under a white lamp: one grey, and somewhere near what two
# bounces come to at the middle of a wall.
if(mean LESS 0.10 OR mean GREATER 0.30)
    message(FATAL_ERROR "baked room gate: the mean of a furnace's light is ${mean}, which is not a furnace's")
endif()
picture(level said low "{\"ambient\":${mean}}")
if(skipped)
    return()
endif()
ask(flat --tolerance=3 "--against=${OUTPUT}/level.png" ${places})
# With no file the same machine has nothing for the room: it is the room with
# no ambient at all, and darker than the one above.
picture(nothing said low "{\"volume\":true}")
if(skipped)
    return()
endif()
picture(dark said low "{\"ambient\":0}")
if(skipped)
    return()
endif()
ask(nothing --tolerance=2 "--against=${OUTPUT}/dark.png" ${places})

# --- Refused, and said -----------------------------------------------------------
picture(other said high "{\"volume\":true,\"baked\":\"${BAKED}\",\"spacing\":2}")
if(skipped)
    return()
endif()
must_say("${said}" "app.warn.irradiance_baked_other" "a volume of another spacing took the file's light")
must_not_say("${said}" "app.info.irradiance_kept" "a volume of another spacing took the file's light")

picture(changed said high "{\"volume\":true,\"baked\":\"${BAKED}\",\"pillar\":true}")
if(skipped)
    return()
endif()
must_say("${said}" "app.warn.irradiance_baked_stale" "a room with a pillar that was not there was not said to have changed")
# The file's light is drawn all the same.
must_say("${said}" "app.info.irradiance_kept" "a room that changed did not keep its file's light")

message("baked room: a volume is baked to a file, read back as the light the rays made, drawn as its mean where no volume is worked out, and refused where it is another's")
