# The white furnace (ADR 0203), asked of pictures of one room: shut on
# every side, every wall the same grey (half of what lands on it given back),
# one lamp at its middle. What such a room's walls give back is known without
# drawing anything -- by radiosity over the same lamp's falloff it is 0.158 of
# a white surface's light at the middle of a wall after two bounces -- so:
#
#   bounced   the room with an `IrradianceVolume` of two bounces;
#   ambient   the same room with no volume, and `Ambient` put to 0.158.
#
# **The two are the same picture**, to a few levels of 255, at the middle of
# the wall, beside it, at the ceiling and at the floor. A volume that handed a
# room more light than its lamp put in -- or less -- would part them: this is
# what holds the whole way from a ray to a pixel (the rays, the lamp's
# falloff, the bounces, what is uploaded, what the shader reads) to the
# arithmetic.
#
#   tinted    the volume with a `Tint` of a half, against an ambient of half
#             of 0.158; and
#   added     the volume in `BounceOnly` over an ambient of 0.1, against an
#             ambient of 0.258: the authored ambient is kept.
#
#   up        the ceiling straight above the lamp. A light that shines
#             every way lights what is behind it: the ceiling there is the
#             brightest part of the ceiling, not a black disc.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_furnace_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_furnace_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")

# As `run_split_screen_gate.cmake` has it, and for its reason.
function(device_was_removed out)
    set(${out} FALSE PARENT_SCOPE)
    if(NOT host_output MATCHES "0x887A0005|device instance has been suspended")
        return()
    endif()
    message("${host_output}")
    string(REGEX MATCH "\\[crash\\] wrote ([^\n\r]*\\.txt)" _ "${host_output}")
    if(CMAKE_MATCH_1 AND EXISTS "${CMAKE_MATCH_1}")
        file(READ "${CMAKE_MATCH_1}" report)
        message("--- the host's crash report (${CMAKE_MATCH_1}) ---\n${report}\n--- end of it ---")
    endif()
    message("ENG_TEST_SKIP: the graphics device was removed under the test")
    set(${out} TRUE PARENT_SCOPE)
endfunction()

# One picture, small: what is asked of it is flat walls.
function(picture name data)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=40 --exit --locale=en --width=480 --height=270
                "--screenshot=${OUTPUT}/${name}.png" "--scene-data=${data}"
                --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows --no-anti-aliasing
                --global-illumination=high
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
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
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${name}.png")
        message("${host_output}")
        message(FATAL_ERROR "furnace gate: the host exited ${host_result} and wrote no picture of `${name}`")
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
        message(FATAL_ERROR "furnace gate: `${name}` is not as it should be (see above); it is in ${OUTPUT}")
    endif()
endfunction()

set(skipped FALSE)
picture(up "{\"look\":\"up\"}")
if(skipped)
    return()
endif()
# Straight above the lamp against a metre and a half to each side of that.
ask(up --tolerance=6 "0.500,0.500>0.250,0.500" "0.500,0.500>0.750,0.500")

picture(ambient "{\"ambient\":0.158}")
if(skipped)
    return()
endif()
picture(bounced "{\"volume\":true}")
if(skipped)
    return()
endif()
# The wall ahead: its middle, and to each side of it. Then the ceiling and
# the floor.
ask(bounced --tolerance=5 "--against=${OUTPUT}/ambient.png"
    "0.450,0.450:0.550,0.550==" "0.300,0.400:0.400,0.600==" "0.600,0.400:0.700,0.600=="
    "0.450,0.060:0.550,0.120==" "0.450,0.900:0.550,0.960==")

# **A tint is a multiplication**: the volume's light at half is the ambient
# at half of 0.158.
picture(half "{\"ambient\":0.079}")
if(skipped)
    return()
endif()
picture(tinted "{\"volume\":true,\"tint\":0.5}")
if(skipped)
    return()
endif()
ask(tinted --tolerance=5 "--against=${OUTPUT}/half.png"
    "0.450,0.450:0.550,0.550==" "0.300,0.400:0.400,0.600==" "0.450,0.060:0.550,0.120==" "0.450,0.900:0.550,0.960==")

# **What bounces alone is added to an authored ambient, which is kept**
# (`Enum.IrradianceMode.BounceOnly`): a tenth of ambient and the volume are
# the room with no volume and an ambient of 0.258. In the other mode the
# volume would have taken the ambient's place, and the room be a tenth darker.
picture(both "{\"ambient\":0.258}")
if(skipped)
    return()
endif()
picture(added "{\"volume\":true,\"bounceOnly\":true,\"ambient\":0.1}")
if(skipped)
    return()
endif()
ask(added --tolerance=5 "--against=${OUTPUT}/both.png"
    "0.450,0.450:0.550,0.550==" "0.300,0.400:0.400,0.600==" "0.450,0.060:0.550,0.120==" "0.450,0.900:0.550,0.960==")

message("furnace: a closed room gives back what radiosity says it does, tinted and added as asked, and a lamp lights the ceiling above it")
