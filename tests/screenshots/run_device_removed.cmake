# A graphics device taken away underneath the host is survived.
#
# Three times in two days a hosted runner's software device was removed under
# a test -- `DXGI_ERROR_DEVICE_REMOVED` -- and each time the host said "Failed
# to map buffer pool" twice and died of an access violation inside the
# graphics library: binding a pipeline had left its uniform buffer slots
# empty, and the draw that followed read through them (SDL patch 0004).
#
# Here the device is removed for real (`--remove-device=N`: the driver's own
# removal, under the backend, with nothing above told), a third of the way
# through `examples/36-face` -- skinned and morphed characters, which is what
# all three hosts were drawing. What is held:
#
#   - the host does not die: no crash report is written;
#   - it ends with the code of a lost device, having said so;
#   - and, where the device is Direct3D 12's, it says the one sentence a test
#     whose device was taken away is known by -- the sentence every test that
#     draws is skipped on (`cmake/engine_device_tests.cmake`), handed in here
#     as TAKEN so the two cannot come apart.
#
# On another graphics API the removal is the simulated one, which the host
# was told of: it ends the same way and does not say the system did it.
#
# Not one of the class that skips on that sentence, for the plain reason that
# this test prints it on purpose.
#
# **And taken away as a picture is read back** (`-DDRIVE=<file> -DSHOTS=<dir>`,
# the fourth sighting): a drive whose `shot` falls on the frame after the
# removal. A hosted runner took a device at that call, and the host answered
# that the picture could not be read -- an error, exit 1 -- where it should
# have said what every other test of the class says. Held the same way: the
# code of a lost device, the sentence, and no "could not read".
#
# **And taken away where nothing meets it** (`-DFRAMES=30`: on the run's last
# frame, with no call after it to fail). The host ended well, exit 0, having
# drawn nothing it could vouch for. Every session asks its device as it ends,
# whatever it is about to report -- the one rule every path passes -- and this
# is the path nobody listed.
#
# **And nothing is asked of a device that is gone** (the fifth sighting: this
# test's own host, dead on a hosted runner inside the graphics library). A
# frame is made of calls that return nothing; drawn on a removed device, each
# asks the library for something it can no longer make, and the library says
# so -- a hundred and sixty-two times in the frame after the removal here --
# and, when the one it could not make was a descriptor heap, read through it
# and died. Which call meets the removal first is a race, so the host could
# not be held to an order: it is held to there being no such call. A frame
# begins by asking the driver whether the device is there, and the frame after
# a removal draws nothing. Held by what the library itself prints, which it
# prints in the builds whose device is made with its debug mode; a build
# without it prints nothing either way, and the rest of this file holds it.
#
#     cmake -DHOST=... -DSCRIPT=<examples/36-face> -DTAKEN=<the sentence> -P ...

set(ENG_NO_DEVICE_EXIT_CODE 4)
set(ENG_DEVICE_LOST_EXIT_CODE 5)
# The host is read by the keys of what it says, not by the words (`--log-keys`).
set(ENV{ENG_LOG_KEYS} 1)
foreach(required HOST SCRIPT TAKEN)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_device_removed.cmake: -D${required}=... is required")
    endif()
endforeach()

set(shots "")
if(DEFINED DRIVE)
    file(REMOVE_RECURSE "${SHOTS}")
    set(shots "--editor-drive=${DRIVE}" "--editor-shots=${SHOTS}")
endif()
if(NOT DEFINED FRAMES)
    set(FRAMES 100)
endif()
execute_process(
    COMMAND "${HOST}" "${SCRIPT}" --headless --frames=${FRAMES} --exit --pace=60 --width=320 --height=180
            --quality=low --remove-device=30 ${shots}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
message("${output}")
if(result EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_NO_DEVICE_HERE: no graphics device on this machine")
    return()
endif()
if(output MATCHES "\\[crash\\] wrote")
    message(FATAL_ERROR "device removed: the host died of it (exit ${result}); its report is named above")
endif()
if(output MATCHES "Failed to map buffer pool|Could not create buffer|Failed to create descriptor heap")
    message(FATAL_ERROR "device removed: the host went on asking the graphics library for buffers and heaps on a "
                        "device that was gone -- calls that return nothing, one of which the library does not "
                        "survive. The frame after a removal is to be an empty one (`IDevice::beginFrame`).")
endif()
if(NOT result EQUAL ENG_DEVICE_LOST_EXIT_CODE)
    message(FATAL_ERROR "device removed: the host exited ${result}, where a lost device is ${ENG_DEVICE_LOST_EXIT_CODE}")
endif()
if(output MATCHES "readback_failed")
    message(FATAL_ERROR "device removed: the host said a picture could not be read back, where its device had "
                        "been taken away")
endif()
if(NOT output MATCHES "engine\\.err\\.device_lost")
    message(FATAL_ERROR "device removed: the host ended without saying its device was lost")
endif()
if(output MATCHES "through direct3d12")
    if(NOT output MATCHES "${TAKEN}")
        message(FATAL_ERROR "device removed: the host did not say the system took its device away -- the sentence a "
                            "test whose device goes is skipped on (\"${TAKEN}\")")
    endif()
    message("device removed: survived, and said the system took it away")
else()
    message("device removed: survived (a simulated loss here: this device is not Direct3D 12's)")
endif()
