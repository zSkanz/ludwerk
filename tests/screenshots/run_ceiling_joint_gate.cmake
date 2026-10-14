# No line of sun where a room's wall meets its ceiling.
#
# The sun's shadow map is compared with a bias, so a surface does not shadow
# itself -- a constant, and a term that grows with the slope the light meets
# the surface at. On a wall the sun grazes that is eight texels along the
# light, and the filter reads a few more to either side: under whatever casts
# on that wall, the map leaves a band lit. Under a ceiling it is a band of
# full sun along the top of a wall that stands in shadow.
#
# The contact mask is what closes it -- a short ray marched through the depth
# buffer towards the sun, for exactly the contact a shadow map loses. But it
# faded from its ray's first step, `1 - t * t`, and its ray was a fixed 0.6 m:
# where the band was as long as the ray, the far end of it kept a fifth of the
# sun. On the wall of a room lit by a window that is a thin pale line under
# the ceiling, on every wall the sun would graze if the ceiling were not
# there. The mask is now whole for as far as the cascade's own bias and filter
# reach, and its ray is half as long again as that.
#
# Asked of `examples/39-interior`, where it was seen: the joint of the back
# wall and of the left wall with the ceiling, at High and at Ultra. A pixel
# that stands out from both its neighbours six pixels away -- up and down, or
# left and right -- is one of the line (`imgprobe`'s `^N/L`): the wall under
# a ceiling darkens towards it or stays as it is, and is never a stripe. The
# corrected mask leaves none in the 19,851 pixels asked; the one before it
# left 1,182 on the back wall and 254 on the left one.
#
# **And asked where nothing hides it**. The mask is a patch over the
# band, and it is not there at `Low` nor on a phone: with contact shadows off
# the line is a band of full sun, twenty-one rows of 720 tall at `Low` and ten
# at `Ultra`. The band is the bias's, and is closed where it is made: a
# receiver that is under a caster is compared away from the light and not
# towards it (`shadowReference`, `engine_brdf.hlsli`). So the same two joints
# are asked at `Low` and at `Ultra` with `--no-contact-shadows`, by a comb a
# band's width wide: 1,705 and 714 teeth at `Low` before, 34 and none after;
# 1,280 and 676 at `Ultra` before, 43 and 2 after -- what is left is single
# pixels along the joint itself, where the band was -- held at a hundred.
#
# **Two tests, and no frame that is not needed**. These were one test
# of four runs of 240 frames, and a frame of this room on a hosted runner's
# software device is over a second: 613 and 619 seconds when it was two runs,
# 701 with four, and past the class's ceiling of 900 on the run after -- a
# gate that passes or fails by how busy the runner's neighbours are. What
# each run waits for was measured (`-DPART`):
#   masked   High and Ultra with the contact mask. High gathers nothing from
#            the screen, and its joints are the same picture at thirty frames
#            as at 240: it runs thirty. Ultra's bounce has to have filled for
#            the ceiling to be brighter than the wall under it -- which is
#            how the gate knows it is asking at the joint -- and that is not
#            so at 60 frames nor at 90, is by 26 at 120, by 33 at 150 and by
#            39 at 240 (fifteen is asked): it runs 150. No tooth at any.
#   bare     Low, and Ultra with no contact shadow. Low is the same at twenty
#            frames as at 240 (35 teeth and 34) and runs thirty. Ultra's
#            count falls as its bounce fills -- 82 at twenty frames, 67 at 60,
#            51 at 120, 36 at 150, 43 at 240, held at a hundred -- and it
#            runs 150.
# As two tests of 300 frames they took 374 and 386 seconds on a hosted
# runner: over a third of the ceiling still, which is where a slow day's run
# is a failure. At 180 frames a test that is 250.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<examples/39-interior> -DPART=<masked|bare>
#         -DOUTPUT=<png, the quality is written into its name> -P run_ceiling_joint_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT PART)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_ceiling_joint_gate.cmake: -D${required}=... is required")
    endif()
endforeach()
if(NOT PART STREQUAL "masked" AND NOT PART STREQUAL "bare")
    message(FATAL_ERROR "run_ceiling_joint_gate.cmake: -DPART is `masked` or `bare`, not `${PART}`")
endif()

# (No variable here is called `masked` or `bare`: a CMake that reads a quoted
# word as the variable of that name -- the container's does, in a script --
# would compare `PART` with a list and run the wrong part without a word.)
set(with_mask high ultra)
set(without_mask low ultra-bare)
if(PART STREQUAL "masked")
    set(without_mask "")
else()
    set(with_mask "")
endif()

foreach(quality IN LISTS with_mask)
    string(REPLACE ".png" "-${quality}.png" picture "${OUTPUT}")
    file(REMOVE "${picture}")
    # Ultra waits for the bounce it gathers from the screen: until it has
    # filled, the ceiling is not yet brighter than the wall under it. High
    # gathers none. (The counts are the header's.)
    set(frames 150)
    if(quality STREQUAL "high")
        set(frames 30)
    endif()
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=${frames} --exit --locale=en --width=1280 --height=720
                --quality=${quality} "--screenshot=${picture}"
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    message("${host_output}")
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${picture}")
        message(FATAL_ERROR "ceiling joint gate: at ${quality} the host exited ${host_result} and wrote no picture")
    endif()

    # x,y as fractions of 1280 by 720.
    execute_process(
        COMMAND "${PROBE}" "${picture}" --tolerance=15
            # The back wall under the ceiling, and the left wall under it.
            "0.258,0.264:0.414,0.333^0/6"
            "0.148,0.083:0.227,0.222^0/6"
            # And the regions are those joints: the ceiling is lit by the
            # room, and brighter than the top of the wall that meets it.
            "0.273,0.236>0.273,0.292"
            "0.125,0.139>0.192,0.139"
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR
            "ceiling joint gate: at ${quality} there is a line where a wall meets the ceiling, or the joint is not "
            "where it was asked (see above).\n"
            "The shadow map's bias leaves a band lit under a caster, and the contact mask has to close all of it. "
            "The picture is ${picture}.")
    endif()
endforeach()

foreach(run IN LISTS without_mask)
    if(run STREQUAL "low")
        set(asked --quality=low)
        set(reach 12)
        set(frames 30)
    else()
        set(asked --quality=ultra --no-contact-shadows)
        set(reach 6)
        set(frames 150)
    endif()
    string(REPLACE ".png" "-${run}.png" picture "${OUTPUT}")
    file(REMOVE "${picture}")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=${frames} --exit --locale=en --width=1280 --height=720
                ${asked} "--screenshot=${picture}"
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${picture}")
        message("${host_output}")
        message(FATAL_ERROR "ceiling joint gate: ${run}: the host exited ${host_result} and wrote no picture")
    endif()
    execute_process(
        COMMAND "${PROBE}" "${picture}" --tolerance=15
            "0.258,0.264:0.414,0.333^100/${reach}"
            "0.148,0.083:0.227,0.222^100/${reach}"
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${run}: ${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR
            "ceiling joint gate: ${run}: there is a band of sun along the top of a wall under its ceiling, with no "
            "contact shadow to hide it (see above).\n"
            "A receiver under a caster is being compared towards the light. The picture is ${picture}.")
    endif()
endforeach()
if(PART STREQUAL "masked")
    message("ceiling joint gate: no line under the ceiling at High and at Ultra")
else()
    message("ceiling joint gate: no band under the ceiling with nothing to hide one, at Low and at Ultra")
endif()
