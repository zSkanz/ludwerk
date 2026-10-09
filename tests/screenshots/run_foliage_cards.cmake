# A card's shadow has the card's holes (terrain audit T3).
#
# The project is copied twice, one of its `variants/` put in as the card's
# image each time: one with its left half cut away (alpha under the material's
# cutoff) and one whole. Each is photographed from above as the sun's shadow
# on the ground (`--debug-view=shadow`), and `imgshadow` weighs the shadow the
# map casts there -- its mass, a penumbra's pixel counting for what it darkens,
# because the cascade blurs a card's shadow past any count of dark pixels.
#
# **The cut card must cast about half the shadow**: 0.47 of it, measured, and
# under 0.7 passes. The shadow pass read no alpha, and cast both the same.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DSHADOW=<imgshadow> -DSCRIPT=<project>
#         -DOUTPUT=<dir> -P run_foliage_cards.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST SHADOW SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_foliage_cards.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")

foreach(variant holes solid)
    set(project "${OUTPUT}/${variant}")
    # The scene's own compiled store is left behind too: somebody who ran
    # the scene where it lies leaves one, and a copy that carried it measured
    # the card that somebody had drawn, twice.
    file(COPY "${SCRIPT}/" DESTINATION "${project}" PATTERN "variants" EXCLUDE PATTERN ".engine" EXCLUDE)
    file(COPY_FILE "${SCRIPT}/variants/${variant}.png" "${project}/content/models/card.png")
    execute_process(
        COMMAND "${HOST}" "${project}" --headless --frames=60 --exit "--screenshot=${OUTPUT}/${variant}.png"
                --debug-view=shadow
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        message("ENG_TEST_SKIP: no graphics device on this machine")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${variant}.png")
        message("${output}")
        message(FATAL_ERROR "foliage cards: the host failed rendering ${variant} (${result})")
    endif()
    execute_process(
        COMMAND "${SHADOW}" "${OUTPUT}/${variant}.png" "--max-map=100000000"
        OUTPUT_VARIABLE measured
        ERROR_VARIABLE measured)
    message("${measured}")
    string(REGEX MATCH "map mass ([0-9]+)" found "${measured}")
    if(NOT found)
        message(FATAL_ERROR "foliage cards: imgshadow printed no mass for ${variant}")
    endif()
    set(mass_${variant} "${CMAKE_MATCH_1}")
endforeach()

if(mass_solid LESS 1000)
    message(FATAL_ERROR "foliage cards: the whole card cast almost no shadow (${mass_solid}); the scene is wrong")
endif()
# holes / solid under 0.7, in integers.
math(EXPR scaled "${mass_holes} * 10")
math(EXPR limit "${mass_solid} * 7")
if(NOT scaled LESS limit)
    message("ENG_FOLIAGE_CARDS_FAULT")
    message(FATAL_ERROR "foliage cards: a card half cut away cast ${mass_holes} of the ${mass_solid} the whole one did")
endif()
