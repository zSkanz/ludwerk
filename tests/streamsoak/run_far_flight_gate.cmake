# The far flight's gate (ADR 0150), in three steps: lay a world, check what was
# made of it, fly over it.
#
# **What it gates is what does not depend on the machine.** That the import
# lays ground wider than one table and leaves the far ground's files behind it
# -- a top-level file a block, counted -- and that a flight over the result,
# out and back twice:
#
#   - streamed the ground in and let it go again, `MIN_GROUND` cells each way
#     (a flight that flew over nothing has the best numbers of all);
#   - drew its far ground from those files, more of them than there are
#     blocks, and read no cell whole to make one again;
#   - held on its second lap what it held on its first, within `GROWTH` per
#     cent, and stayed under a ceiling -- a flight that keeps what it has
#     flown over holds a lap more each time: three leaks of exactly that kind
#     were found by this flight, measured by hand, before it was a gate (D405,
#     D406, D408), and none of them would have reached a ceiling wide enough
#     for a software device;
#   - and never spent a frame's worth of time inside streaming.
#
# Frame times are in `docs/perf-baselines.md`, where a human put them: the
# whole frame here is the device's -- 24 ms at the median on lavapipe, 1.6 ms
# on a GPU -- and what is asserted of it is that it did not stall.
cmake_minimum_required(VERSION 3.24)

set(ENG_NO_DEVICE_EXIT_CODE 4)

foreach(required HOST PROJECT WORK REPORT SIZE BLOCKS FRAMES CEILING_MB MIN_GROUND GROWTH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_far_flight_gate.cmake needs -D${required}=")
    endif()
endforeach()

# A copy in the build tree, fresh: the import writes the scene and its cells
# into the project it is given, and the repository's copy has neither.
file(REMOVE_RECURSE "${WORK}")
file(COPY "${PROJECT}/" DESTINATION "${WORK}")

execute_process(
    COMMAND "${HOST}" "${WORK}"
        --import-terrain=hills --import-size=${SIZE} --import-low=0 --import-high=120 --import-scale=600
    RESULT_VARIABLE importResult
    OUTPUT_VARIABLE importOutput
    ERROR_VARIABLE importOutput)
message(STATUS "${importOutput}")
if(NOT importResult EQUAL 0)
    message(FATAL_ERROR "the import failed (${importResult})")
endif()

# The far ground's files: a top-level one for every block the ground reaches.
file(GLOB topFiles "${WORK}/.engine/terrain-pyramid/*/L5/*.lnode")
list(LENGTH topFiles topCount)
if(topCount LESS ${BLOCKS})
    message(FATAL_ERROR "the import left ${topCount} top-level files of the far ground; ${BLOCKS} blocks were laid")
endif()
file(GLOB cellFiles "${WORK}/content/terrain/scenes/main.scene/*.lterrain")
list(LENGTH cellFiles cellCount)
message(STATUS "far flight: ${cellCount} cells, ${topCount} blocks of far ground")

# **On a real device, unlike the soaks beside it**: `--rhi=null` has no
# renderer, and with none nothing asks for a node of the far ground -- the
# first version of this gate ran so, and passed over a flight that read
# thirty-six files and built nothing. Small frames, so a software device is
# not what the run costs.
#
# **Paced at 240**, four times a game's clock: a frame is a sixtieth of a
# second of flight whatever it took, and with nothing holding it a fast
# machine's millisecond frames fly the world in four seconds -- the disk is
# then a lap behind, the second quarter of the run holds half the ground the
# last one does, and the lap-over-lap check fails a flight that leaks nothing
# (seen: 267 MiB against 325, with 2 927 cells in where a paced run has
# 5 295). A slow device is under the pace and not held by it.
execute_process(
    COMMAND "${HOST}" "${WORK}"
        --headless --frames=${FRAMES} --pace=240 --exit --width=320 --height=180
        --soak-report=${REPORT}
        --soak-ceiling-mb=${CEILING_MB}
        --soak-min-ground=${MIN_GROUND}
        --soak-memory-growth=${GROWTH}
        --soak-frame-p99-ms=250
    RESULT_VARIABLE soakResult
    OUTPUT_VARIABLE soakOutput
    ERROR_VARIABLE soakOutput)
message(STATUS "${soakOutput}")
# **No device, no flight** -- and said, not passed: the hosted Linux runner has
# no GPU and no software one either, and the first push of this gate was red
# there. The import and the count of its files above have run by now, on the
# null device an import takes (D413); what is skipped is the flight.
if(soakResult EQUAL ENG_NO_DEVICE_EXIT_CODE)
    message("ENG_TEST_SKIP: no graphics device on this machine")
    return()
endif()
if(NOT soakResult EQUAL 0)
    message(FATAL_ERROR "the far flight failed its gate (${soakResult}); the report is at ${REPORT}")
endif()

# The far ground was drawn from its files, and none was made again.
file(READ "${REPORT}" report)
string(JSON filesRead GET "${report}" farGroundFiles)
string(JSON cellsRead GET "${report}" farGroundCellsRead)
string(JSON cellsIn GET "${report}" groundCellsIn)
string(JSON cellsOut GET "${report}" groundCellsOut)
message(STATUS "far flight: ${cellsIn} cells in, ${cellsOut} out, ${filesRead} far files read, ${cellsRead} cells read again")
if(NOT filesRead GREATER topCount)
    message(FATAL_ERROR "the flight read ${filesRead} of the far ground's files over ${topCount} blocks; its far ground was not drawn from them")
endif()
if(NOT cellsRead EQUAL 0)
    message(FATAL_ERROR "the flight read ${cellsRead} cells whole to make far ground the import had made")
endif()
