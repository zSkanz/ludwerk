# `BasePart.CastShadow`, and the sun's shadow under an orthographic camera
# (D536), on a real device.
#
# Two slabs float over a floor, seen from straight above by an orthographic
# camera thirty metres up: the left one casts a shadow and the right one was
# asked to cast none. Two frames of one scene:
#
#   the sun    the floor where the left slab's shadow falls is darker than the
#              floor the same way from the right slab;
#   a lamp     the same under a spot lamp above each: the lamps' shadow passes
#              read the property too.
#
# And both slabs are drawn in both: the property takes away what a part casts
# and nothing else.
#
# **The first half is D536's test.** Before it the sun's frame had no shadow
# at all: under an orthographic camera every fragment chose the first cascade.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROBE=<imgprobe> -DSCRIPT=<castshadow>
#         -DOUTPUT=<directory> -P run_cast_shadow_gate.cmake

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROBE SCRIPT OUTPUT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_cast_shadow_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUTPUT}")

# The scene shows the sun before frame 30 and the lamps from it.
function(shoot name frames)
    file(REMOVE "${OUTPUT}/${name}.png")
    execute_process(
        COMMAND "${HOST}" "${SCRIPT}" --headless --frames=${frames} --exit "--screenshot=${OUTPUT}/${name}.png"
                --width=1000 --height=500 --no-auto-exposure --no-bloom --no-ambient-occlusion --no-contact-shadows
        RESULT_VARIABLE host_result
        OUTPUT_VARIABLE host_output
        ERROR_VARIABLE host_output)
    if(host_result EQUAL ENG_NO_DEVICE_EXIT_CODE)
        set(no_device TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT host_result EQUAL 0 OR NOT EXISTS "${OUTPUT}/${name}.png")
        message("${host_output}")
        message(FATAL_ERROR "cast shadow gate: the host exited ${host_result} and wrote no ${name} picture")
    endif()
endfunction()

function(probe name tolerance)
    execute_process(
        COMMAND "${PROBE}" "${OUTPUT}/${name}.png" --tolerance=${tolerance} ${ARGN}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_output
        ERROR_VARIABLE probe_output)
    message("${name}: ${probe_output}")
    if(NOT probe_result EQUAL 0)
        message(FATAL_ERROR "cast shadow gate: a probe disagreed (see above); the frame is ${OUTPUT}/${name}.png")
    endif()
endfunction()

set(no_device FALSE)
shoot(sun 20)
if(no_device)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
shoot(lamp 50)

# A slab is three metres square at x = -5 and x = 5: 0.25 and 0.75 of the
# picture. The afternoon sun throws the left one's shadow about four metres
# along x; a lamp eight metres up throws it all round the slab, a metre wide.
#
# **A whole shadow, not a hint of one** (D537): the floor is thirty metres from
# the camera, where one cascade hands over to the next, and the slab is three
# metres nearer -- outside the further cascade's sphere, between it and the
# sun. That cascade drew no slab, and the shadow was a third as dark as it is.
# Eighty is past that third and under the whole.
probe(sun 80
    # The left slab's shadow, against the floor the same way from the right one.
    "0.445,0.5<0.945,0.5"
)
probe(lamp 80
    "0.35,0.5<0.85,0.5"
)
# Both slabs are drawn, in both: neither place is the floor's colour.
probe(sun 30
    "0.25,0.5!=135,144,160"
    "0.75,0.5!=135,144,160"
)
probe(lamp 30
    "0.25,0.5!=225,228,240"
    "0.75,0.5!=225,228,240"
)
