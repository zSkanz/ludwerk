# Light that bounces in the open (ADR 0206), asked of pictures of one place:
# a red wall the sun is on, and a canopy beside it with the ground under it in
# shadow. The sky and the flat ambient are put out, so the ground under the
# canopy is lit by what the wall gives back and by nothing else.
#
#   none     no volume: under the canopy is black;
#   follow   an `IrradianceVolume` that `FollowsCamera`, three levels: under
#            the canopy is lit, and red;
#   site2    the same place three hundred and twenty metres on, with the camera there:
#            the same picture -- the volume is where the camera is;
#   walk     the camera goes sixty metres to the first place, a step a frame,
#            with the frame's budget for bounced light at a millisecond and a
#            half: the levels are carried after it -- the log says so -- and
#            the picture when it has arrived is the picture of standing there;
#   tiles    no camera-following volume: one box over the place, too long
#            for one volume at its spacing, worked out as two tiles. Lit,
#            with no line where the two meet (the seam runs down the middle
#            of the picture).
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_open_field_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

# The host is read by the keys of what it says, not by the words (`--log-keys`).
set(ENV{ENG_LOG_KEYS} 1)
foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_open_field_gate.cmake: -D${required}=... is required")
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

# One picture, small. `frames` and any flags of the host's after the data.
function(picture name data frames)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit --locale=en --width=320 --height=180
                "--screenshot=${OUTPUT}/${name}.png" "--scene-data=${data}"
                --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows --no-anti-aliasing
                --global-illumination=high --reflections=low ${ARGN}
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    set(last_output "${host_output}" PARENT_SCOPE)
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
        message(FATAL_ERROR "open field gate: the host exited ${host_result} and wrote no picture of `${name}`")
    endif()
    if(host_output MATCHES "\\[error\\]|\\[warn\\]")
        message("${host_output}")
        message(FATAL_ERROR "open field gate: the host warned or erred drawing `${name}`")
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
        message(FATAL_ERROR "open field gate: `${name}` is not as it should be (see above); it is in ${OUTPUT}")
    endif()
endfunction()

set(skipped FALSE)
# Under the canopy, in two places; and the ground in the sun before it, which
# is lit whatever is done.
picture(none "{}" 40)
if(skipped)
    return()
endif()
ask(none --tolerance=6 "0.500,0.620=0,0,0" "0.300,0.620=0,0,0" "0.500,0.950!=0,0,0")

picture(follow "{\"follow\":true}" 120)
if(skipped)
    return()
endif()
ask(follow --tolerance=8 "0.500,0.620!=0,0,0" "0.300,0.620!=0,0,0")

# Three hundred and twenty metres on: the same picture.
picture(site2 "{\"follow\":true,\"site\":2}" 120)
if(skipped)
    return()
endif()
ask(site2 --tolerance=4 "--against=${OUTPUT}/follow.png" "0.150,0.550:0.850,0.720==")

# Walked to: carried, and the same picture once there. **Nothing here is a
# clock**: every frame does at least a piece of the work whatever the
# machine, a level of four hundred points is fifteen pieces at most, and the
# frames each picture is given are several times what its levels can take.
picture(walk "{\"follow\":true,\"walk\":true}" 420 --global-illumination-budget=1.5)
if(skipped)
    return()
endif()
if(NOT last_output MATCHES "engine.info.indirect_light_carried")
    message("${last_output}")
    message(FATAL_ERROR "open field gate: sixty metres were walked and no level was carried after the camera")
endif()
ask(walk --tolerance=6 "--against=${OUTPUT}/follow.png" "0.150,0.550:0.850,0.720==")

# A box too large for one volume, as tiles: lit as the other is, the seam
# between two tiles down the middle of the picture and not to be seen.
# (That it IS tiles is that nothing warned: a box thirty-six points long
# worked out as one volume is given a coarser spacing, and says so.)
picture(tiles "{\"tiles\":true}" 150)
if(skipped)
    return()
endif()
ask(tiles --tolerance=8 "0.500,0.620!=0,0,0" "0.300,0.620!=0,0,0" "0.700,0.620!=0,0,0")
# Across the seam, a band of the ground at one distance from the wall is one
# colour from end to end: a line where the tiles meet would be another.
ask(tiles --tolerance=5 "0.420,0.635:0.580,0.645~18,1,0")

message("open field: lit by a volume carried with the camera, the same three hundred and twenty metres on and after a walk, and by tiles")
