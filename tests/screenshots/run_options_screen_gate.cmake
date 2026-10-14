# Does the ready-made options screen (`@engine/settings`, ADR 0147 section 7)
# choose what is drawn, and is it drawn where it says?
#
# Two claims, from one scene (`tests/screenshots/optionsscreen`) whose script
# steps the screen's own quality row the way a player's presses do:
#
#   1. A picture per level. The world at `Low`, `Medium`, `High` and `Ultra`,
#      each chosen through the screen, and no two neighbouring levels the
#      same picture -- a differential, as `run_settings_differential.cmake` is
#      and for its reason: a level that is accepted and reaches nothing that
#      draws looks exactly like one that works. The first and the last are
#      also the picture of a game STARTED at that level; the two between are
#      `graphics_runtime_differential`'s claim already, and each comparison is
#      two more runs on a machine that draws in software.
#   2. The screen itself, by probes: the page that is shown, the button that
#      says there is something to apply, a setting that is on, and the second
#      page after it is turned to.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DIMGCMP=<imgcmp> -DPROBE=<imgprobe> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_options_screen_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST IMGCMP PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_options_screen_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

function(render out frames)
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless "--frames=${frames}" --exit "--screenshot=${out}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    message("${output}")
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "options screen gate: the host exited ${result} (${frames} frames ${ARGN})")
    endif()
    if(NOT EXISTS "${out}")
        message(FATAL_ERROR "options screen gate: no file written (${frames} frames ${ARGN})")
    endif()
    set(rendered "${output}" PARENT_SCOPE)
endfunction()

# --- 1. A picture per level ---------------------------------------------------

set(levels low medium high ultra)
# Six frames after each step of the row, which is when a level is the picture
# of that level. The walk was twenty frames a step and its pictures at 15 to
# 105: four hundred and sixty-five frames in all, on a hosted runner whose
# device is the processor, for claims none of which is about time.
set(frames 6 14 22 30)
set(previous "")
# **One anti-aliasing for every picture here** (ADR 0158): `Ultra`'s is the
# temporal pass, whose history begins when it is chosen -- on the screen, a
# few frames before the picture; from the start, seventy frames before it --
# and whose sample positions are counted from then. The two would differ by
# design, in a way that says nothing about whether the level reached what
# draws, which is this gate's question.
#
# **And nothing read from the screen** (ADR 0209), for the same reason: a
# reflection and a bounce are gathered over frames from when they are chosen,
# six frames before one picture and thirty before the other. That a level
# chosen while the game runs reaches them is `screen_light_gate`'s to hold,
# by what the floor shows and not pixel for pixel.
#
# A setting the command line pins is no part of whether the row reads a
# level: until that was so, these flags made this walk go nowhere.
set(SMOOTHING --anti-aliasing=smaa --screen-reflections=low --screen-bounce=low)
foreach(level frame IN ZIP_LISTS levels frames)
    render("${OUTPUT}/${level}-by-screen.png" ${frame} --width=640 --height=360 ${SMOOTHING})
    if(NOT EXISTS "${OUTPUT}/${level}-by-screen.png")
        # The skip was already printed by `render`; there is nothing to compare.
        return()
    endif()
    # **The picture named for a level is of that level**: the scene
    # says where its row is before each picture, and the last it said in this
    # run is this one's. A walk that goes somewhere else writes four files
    # with the right names, and every comparison below can still pass.
    string(SUBSTRING "${level}" 0 1 initial)
    string(SUBSTRING "${level}" 1 -1 rest)
    string(TOUPPER "${initial}" initial)
    string(REGEX MATCHALL "options-screen: the row is at [A-Za-z]+" said "${rendered}")
    list(POP_BACK said last)
    if(NOT last STREQUAL "options-screen: the row is at ${initial}${rest}")
        message(FATAL_ERROR
            "options screen gate: the picture named `${level}` was taken when the scene said \"${last}\".\n"
            "The screen's quality row did not walk to `${level}`.")
    endif()
    if(level STREQUAL "low" OR level STREQUAL "ultra")
        render("${OUTPUT}/${level}-from-start.png" ${frame} --width=640 --height=360 "--quality=${level}"
               ${SMOOTHING})
    endif()

    # The same picture, to within what a frame's history leaves: a shadow
    # cascade keeps the box it had last frame while it can (`shadow.cpp`: that
    # memory is what keeps the texel grid still), so after a level changed the
    # sun's reach while the game ran, a shadow's edge sits a fraction of a
    # texel from where a game started at that level puts it -- 228 pixels of
    # 230 400 at `Ultra` on the machine this was written on, all on shadow
    # edges.
    if(EXISTS "${OUTPUT}/${level}-from-start.png")
        execute_process(
            COMMAND "${IMGCMP}" "${OUTPUT}/${level}-by-screen.png" "${OUTPUT}/${level}-from-start.png"
                    --tolerance 3 --max-different-pixels 512
            RESULT_VARIABLE same_result
            OUTPUT_VARIABLE same_output
            ERROR_VARIABLE same_output)
        message("${same_output}")
        if(NOT same_result EQUAL 0)
            message(FATAL_ERROR
                "options screen gate: `${level}` chosen on the screen is not the picture of a game started at "
                "`${level}`.\nThe screen's quality row reaches the model and not everything that draws.")
        endif()
    endif()

    if(previous)
        # Inverted: a match is the failure.
        execute_process(
            COMMAND "${IMGCMP}" "${OUTPUT}/${previous}-by-screen.png" "${OUTPUT}/${level}-by-screen.png"
                    --tolerance 2 --max-different-pixels 0
            RESULT_VARIABLE differ_result
            OUTPUT_VARIABLE differ_output
            ERROR_VARIABLE differ_output)
        message("${differ_output}")
        if(differ_result EQUAL 0)
            message(FATAL_ERROR
                "options screen gate: `${previous}` and `${level}` are the same picture.\n"
                "A level the screen offers changes nothing that is drawn.")
        endif()
    endif()
    set(previous ${level})
endforeach()

# --- 2. The screen itself ------------------------------------------------------

# Each probe is x,y as fractions of the picture, then the colour. The window is
# 860 by 633.6 in the middle of 1280 by 720, with 18 of padding; the scene's
# theme says which colour is which thing.
render("${OUTPUT}/graphics-page.png" 38 --width=1280 --height=720)
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/graphics-page.png"
        # The backdrop outside the window, and the window.
        "0.078,0.5=10,10,10"
        "0.172,0.556=40,40,60"
        # The page shown is the first: its button is the accent, the other's is not.
        "0.203,0.178=0,200,0"
        "0.547,0.178=90,90,90"
        # Something was stepped, so Apply says there is something to apply; Back is a button.
        "0.195,0.883=0,200,0"
        "0.805,0.883=90,90,90"
        # The first row's "less" button, and the eighth row's -- terrain detail, a
        # stepper since the anti-aliasing rows came in above it (ADR 0158);
        # a setting that is on is the second page's vertical sync, below.
        "0.5,0.254=90,90,90"
        "0.508,0.74=90,90,90"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR
        "options screen gate: a probe of the first page disagreed (see above); the frame is "
        "${OUTPUT}/graphics-page.png")
endif()

render("${OUTPUT}/display-page.png" 46 --width=1280 --height=720)
execute_process(
    COMMAND "${PROBE}" "${OUTPUT}/display-page.png"
        # The second page's button is the accent now.
        "0.203,0.178=90,90,90"
        "0.547,0.178=0,200,0"
        # Its third row is vertical sync, on; and where the first page had an
        # eighth row this one has none.
        "0.508,0.393=0,200,0"
        "0.508,0.74=40,40,60"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR
        "options screen gate: a probe of the second page disagreed (see above); the frame is "
        "${OUTPUT}/display-page.png")
endif()

message("options screen gate: each level the screen offers is that level's picture, and the screen is where it says")
