# Does the ready-made options screen (`@engine/settings`, ADR 0147 section 7)
# choose what is drawn, and is it drawn where it says?
#
# Two claims, from one scene (`tests/screenshots/optionsscreen`) whose script
# steps the screen's own quality row the way a player's presses do:
#
#   1. A picture per level. The world at `Low`, `Medium`, `High` and `Ultra`,
#      each chosen through the screen, is the picture of a game STARTED at that
#      level -- and no two neighbouring levels are the same picture. A
#      differential, as `run_settings_differential.cmake` is and for its
#      reason: a level that is accepted and reaches nothing that draws looks
#      exactly like one that works.
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
endfunction()

# --- 1. A picture per level ---------------------------------------------------

set(levels low medium high ultra)
set(frames 150 300 450 600)
set(previous "")
foreach(level frame IN ZIP_LISTS levels frames)
    render("${OUTPUT}/${level}-by-screen.png" ${frame} --width=640 --height=360)
    if(NOT EXISTS "${OUTPUT}/${level}-by-screen.png")
        # The skip was already printed by `render`; there is nothing to compare.
        return()
    endif()
    render("${OUTPUT}/${level}-from-start.png" ${frame} --width=640 --height=360 "--quality=${level}")

    # The same picture, to within two things a frame's history leaves. The
    # exposure adapts, and the screen's frames spent their first seconds at
    # other levels. And a shadow cascade keeps the box it had last frame while
    # it can (`shadow.cpp`: that memory is what keeps the texel grid still), so
    # after a level changed the sun's reach while the game ran, a shadow's edge
    # sits a fraction of a texel from where a game started at that level puts
    # it -- 228 pixels of 230 400 at `Ultra` on the machine this was written
    # on, all of them on shadow edges, and none at the three levels below.
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
render("${OUTPUT}/graphics-page.png" 640 --width=1280 --height=720)
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
        # The first row's "less" button, and the eighth row -- ambient occlusion, on at Ultra.
        "0.5,0.254=90,90,90"
        "0.508,0.74=0,200,0"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_output
    ERROR_VARIABLE probe_output)
message("${probe_output}")
if(NOT probe_result EQUAL 0)
    message(FATAL_ERROR
        "options screen gate: a probe of the first page disagreed (see above); the frame is "
        "${OUTPUT}/graphics-page.png")
endif()

render("${OUTPUT}/display-page.png" 670 --width=1280 --height=720)
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
