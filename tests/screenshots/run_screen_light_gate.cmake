# What a surface reflects of the screen, and the light that bounces off it
# (ADR 0209), asked of pictures of one project (`tests/screenshots/screenlight`)
# whose scene the command line names. Each claim is one a wrong pass would
# break, and each was seen to fail with its cause switched off:
#
#   mirror   A polished floor under a `ReflectionProbe`, and a red box moved
#            after the probe took its picture. With the screen's reflections
#            on the floor shows the box where it stands now, and the wall
#            where the probe still has the box; with them off it is the
#            probe's picture. And where the screen has no answer -- the floor
#            nearest the camera, whose mirror ray leaves the picture -- the
#            two are the same pixels: the probe's, never black.
#
#   ball     A chrome ball on that floor. A ball is shaded smooth and made of
#            facets; the pass walks its ray from the facets, and a ball
#            reflected through them is cut into flat patches. So a curved
#            surface shaded smooth keeps the probe: inside the ball the
#            picture with reflections on IS the picture with them off. (With
#            that rule switched off, 273 of the 1,521 pixels asked differed.)
#
#   wall     A white floor at the foot of a glowing red wall. With the bounce
#            on the floor is reddened there; with it off it is not. And three
#            more ways of saying "on" and "off": the quality level put to
#            `Ultra` by a script while the game runs reddens it as the flag
#            does -- the level that turns both on, arriving late -- and with
#            the level pinned by the command line the same script changes
#            nothing.
#
#   ground   The same wall on a terrain (ADR 0263): the ground at its foot
#            is reddened with the bounce on and not with it off, as the floor
#            is -- the ground reads the screen as a part does.
#
#   lake     A flat lake, three posts on its far bank and a beam hung over the
#            water in front of them. `imgmirror` holds what the water shows
#            against the scene through the camera mirrored in the water's
#            plane: every pixel the reflection wrote is one where the mirror
#            shows a thing, and went that thing's way. The beam is there for
#            the ray that passes BEHIND it on the way to a post: with the
#            test that a landing must be ON a surface switched off, 3,248 of
#            13,098 reflecting pixels were the beam's colour laid across
#            water whose mirror shows something else.
#
#   off      With both settings off nothing is kept and nothing is copied:
#            the frame has no pass of the screen's light and no copy of the
#            last frame's picture, by the frame's own list of passes. With
#            them on it has both.
#
# Asked of the graphics API the host picks, and of the one named by GPU.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DMIRROR=<imgmirror> -DSCRIPT=<project>
#         -DOUTPUT=<dir> [-DGPU=<driver>] -P run_screen_light_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE MIRROR SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_screen_light_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

set(device "")
if(DEFINED GPU AND NOT GPU STREQUAL "")
    set(device "--gpu=${GPU}")
endif()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

# One picture, 640 by 360. `skipped` is set where there is no device to draw it.
function(picture name frames data)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit --locale=en --width=640 --height=360
                "--screenshot=${OUTPUT}/${name}.png" "--scene-data=${data}" ${device} ${ARGN}
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    message("${host_output}")
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT device STREQUAL "" AND NOT host_output MATCHES ", through ${GPU}\\.")
        message("ENG_TEST_SKIP: no ${GPU} device on this machine; the host drew through another")
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${name}.png")
        message(FATAL_ERROR "screen light gate: the host exited ${host_result} drawing `${name}`")
    endif()
    set(rendered "${host_output}" PARENT_SCOPE)
endfunction()

function(probe what picture)
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${picture}.png" ${ARGN}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR "screen light gate: ${what} (see above); the picture is ${OUTPUT}/${picture}.png")
    endif()
endfunction()

set(ON --quality=high --screen-reflections=ultra --screen-bounce=ultra)
set(OFF --quality=high --screen-reflections=low --screen-bounce=low)

# --- mirror ---------------------------------------------------------------------

picture(mirror-on 36 "{\"scene\":\"mirror\"}" ${ON})
if(skipped)
    return()
endif()
picture(mirror-off 36 "{\"scene\":\"mirror\"}" ${OFF})

# x,y as fractions of 640 by 360. The box's reflection is under it, at the
# right; the place the probe still has it is at the left.
probe("the floor does not show the box where it stands now" mirror-on --tolerance=14
    "0.622,0.639=37,2,18"
    "0.622,0.6=41,2,18"
    # And not where it stood: the wall is there.
    "0.32,0.555!=78,13,31")
probe("with the screen's reflections off the floor is not the probe's picture" mirror-off --tolerance=14
    "0.32,0.555=78,13,31"
    "0.622,0.639!=37,2,18")
# Nearest the camera a mirror ray leaves the picture: the probe's answer, to
# the pixel, and not black.
probe("where the screen has no answer the floor is not the probe's, or is black" mirror-on --tolerance=2
    "--against=${OUTPUT}/mirror-off.png"
    "0.3,0.86:0.7,0.98=="
    "0.5,0.92!=0,0,0")

# --- ball -----------------------------------------------------------------------

picture(ball-on 20 "{\"scene\":\"ball\"}" ${ON})
picture(ball-off 20 "{\"scene\":\"ball\"}" ${OFF})
# The square inside the ball. The floor round it reflects the ball and the
# boxes, and is the screen's; the ball itself is the probe's.
probe("a ball shaded smooth is reflecting through its facets" ball-on --tolerance=1
    "--against=${OUTPUT}/ball-off.png"
    "0.47,0.445:0.53,0.555==")
# And the floor beside it did change: the settings were on. The red box's
# reflection, and the blue one's.
probe("the floor by the ball reflects nothing of the screen" ball-on --tolerance=14
    "0.3625,0.639=37,2,17"
    "0.64,0.611=2,23,64")

# --- wall -----------------------------------------------------------------------

picture(wall-on 40 "{\"scene\":\"wall\"}" ${ON})
picture(wall-off 40 "{\"scene\":\"wall\"}" ${OFF})
picture(wall-late 40 "{\"scene\":\"wall\",\"late\":true}")
picture(wall-pinned 40 "{\"scene\":\"wall\",\"late\":true}" --quality=high)
foreach(lit wall-on wall-late)
    probe("the floor at the wall's foot is not reddened in `${lit}`" ${lit} --tolerance=8
        "0.5625,0.578=199,179,201"
        "0.656,0.597=199,179,201"
        # Away from the wall the floor is the floor.
        "0.3125,0.833=169,180,203")
endforeach()
foreach(unlit wall-off wall-pinned)
    probe("the floor at the wall's foot is reddened in `${unlit}`, with no bounce to redden it" ${unlit}
        --tolerance=8
        "0.5625,0.578!=199,179,201"
        "0.5625,0.578=174,181,204")
endforeach()

# --- ground ---------------------------------------------------------------------

picture(ground-on 40 "{\"scene\":\"ground\"}" ${ON})
picture(ground-off 40 "{\"scene\":\"ground\"}" ${OFF})
probe("the ground at the wall's foot is not reddened in `ground-on`" ground-on --tolerance=8
    "0.5625,0.578=152,122,104"
    "0.656,0.597=157,127,110"
    # Away from the wall the ground is the ground.
    "0.3125,0.833=124,120,102")
probe("the ground at the wall's foot is reddened in `ground-off`, with no bounce to redden it" ground-off
    --tolerance=8
    "0.5625,0.578!=152,122,104"
    "0.5625,0.578=133,123,106")

# --- lake -----------------------------------------------------------------------

picture(lake-on 8 "{\"scene\":\"lake\"}" ${ON})
picture(lake-off 8 "{\"scene\":\"lake\"}" ${OFF})
picture(lake-mirrored 8 "{\"scene\":\"lake\",\"mirror\":true}" ${OFF})
picture(lake-sky 8 "{\"scene\":\"lake\",\"mirror\":true,\"empty\":true}" ${OFF})
# Thirty-two wrong of some ten thousand: the last row of water against the far
# bank, where the water is thin and what it showed before was not the sky.
execute_process(
    COMMAND "${MIRROR}" "${OUTPUT}/lake-on.png" "${OUTPUT}/lake-off.png" "${OUTPUT}/lake-mirrored.png"
            "${OUTPUT}/lake-sky.png" --min-reflecting=4000 --max-wrong=32
    RESULT_VARIABLE mirror_result
    OUTPUT_VARIABLE mirror_output
    ERROR_VARIABLE mirror_output)
message("${mirror_output}")
if(NOT mirror_result EQUAL 0)
    message(FATAL_ERROR
        "screen light gate: the lake does not show what a mirror in its plane shows (see above).\n"
        "Too few pixels reflecting is a reflection that is not there; too many wrong is a reflection of\n"
        "something the ray only passed behind. The pictures are in ${OUTPUT}.")
endif()

# --- off ------------------------------------------------------------------------

picture(passes-off 6 "{\"scene\":\"wall\"}" ${OFF} --gpu-pass-times --frame-report=1)
# By the passes' own names, each followed by its milliseconds: the folder
# these pictures are written to may be named for the gate.
if(rendered MATCHES "screen-light/screen-(trace|bounce) [0-9]|scene-before [0-9]")
    message(FATAL_ERROR
        "screen light gate: with both settings off the frame still has a pass of the screen's light, or still "
        "copies the last frame's picture. Off is nothing kept and nothing copied.")
endif()
picture(passes-on 6 "{\"scene\":\"wall\"}" ${ON} --gpu-pass-times --frame-report=1)
if(NOT rendered MATCHES "screen-light/screen-trace [0-9]" OR NOT rendered MATCHES "screen-light/screen-bounce [0-9]"
   OR NOT rendered MATCHES "scene-before [0-9]")
    message(FATAL_ERROR
        "screen light gate: with both settings on the frame has no pass of the screen's light. Every claim "
        "above about `on` was made of a picture that had none.")
endif()

message("screen light gate: the floor, the ball, the wall, the ground, the lake and the frame with both off all hold")
