# **The tests that need a graphics device, as one class.**
#
# A test that draws carries one mark already: `SKIP_REGULAR_EXPRESSION
# "ENG_TEST_SKIP"`, which is how it says "no device on this machine" without
# failing. That mark IS the class. `eng_mark_device_tests()` gives every test
# of the calling directory that carries it the two things the class shares,
# and takes the names of any others that draw and never skip.
#
#   - **A timeout that is a hang guard, not a budget.** Where the device is a
#     software rasteriser -- a hosted runner's WARP, the container's lavapipe
#     -- a picture is minutes of CPU, and drawn beside three others at `-j 4`
#     it is three times that: a test's time there is a function of what
#     happens to run beside it, not of the test. `terrain_far` passed in
#     100 s alone on CI and was cut at 300 s the first time it ran at `-j 4`;
#     `frame_report_draws_by_kind`, the split-screen gate and
#     `instanced_part_lit_as_alone` each met a ceiling of their own the same
#     way, a run apart, and each had its ceiling raised by itself. The third
#     was the last: the class was a list kept by hand, and every test that
#     met its ceiling was one the list had not been told about. One shard of
#     the hosted Windows run is some 3,700 s of these tests in 1,200 s of
#     clock, four at a time.
#
#     What a ceiling is for is the test that never ends. Fifteen minutes says
#     that, and no test of the class says less.
#
#   - **A turn at the GPU**, where there is one to share: on a real card two
#     hosts drawing at once is the driver's scheduler deciding which stalls,
#     and a gate that measures a frame measures the other test. Where the
#     device is the CPU there is nothing to share but cores, and no group is
#     asked for.
#
#     Three turns at once on a developer's Windows machine
#     (`tests/ctest-resources.json`), and two on the hosted Windows runner
#     (`tests/ctest-resources-hosted.json`, named by the workflow): its
#     software device was taken from under a test four times in two days
#     with three hosts drawing on it at a time. Whether two is
#     fewer removals is read from the count the skip gate prints.
#
#   - **A device taken away is a skip, for every one of them**. A
#     hosted runner's software device was removed under a test three times
#     in two days -- `DXGI_ERROR_DEVICE_REMOVED`, the driver's word that the
#     device is gone and not that anything drawn hung it. The host says so in
#     one sentence when it stops for that in a run that did not ask to lose
#     its device (`engine.info.device_taken_away`), and that sentence in a
#     test's output is the test not having run: no picture to ask anything
#     of. CTest marks it skipped whatever the driver made of the host's exit,
#     and `scripts/gates/assert-no-skips.sh` counts it by name where a run is
#     read. Two picture gates used to say this each for itself; a third test,
#     whose driver did not, failed. The class says it once.
#
#     A driver has to print what its host said for this to be seen -- every
#     one does when its host fails.
#
# Called at the END of a directory's tests: it reads the tests the directory
# has.
option(ENG_TEST_GPU_LOCK "Serialise the tests that draw on a real GPU" ${WIN32})
if(APPLE)
    set(ENG_TEST_GPU_LOCK ON)
endif()
set(ENG_DEVICE_TEST_TIMEOUT 900 CACHE STRING "Seconds before a test that needs a graphics device is taken to hang")
# What the host says when the system removed its device, by the key of the
# sentence and not by its words: a sentence is reworded and translated, and
# a skip that matched the old words would have been a failure on the runner
# it was written for. Every drawn test runs with `ENG_LOG_KEYS`, which puts
# the key before the sentence.
set(ENG_DEVICE_TAKEN_AWAY "engine.info.device_taken_away")

function(eng_mark_device_tests)
    set(class ${ARGN})
    get_property(here DIRECTORY PROPERTY TESTS)
    foreach(candidate IN LISTS here)
        get_property(skips TEST ${candidate} PROPERTY SKIP_REGULAR_EXPRESSION)
        if(skips MATCHES "ENG_TEST_SKIP")
            list(APPEND class ${candidate})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES class)
    foreach(drawn IN LISTS class)
        if(NOT TEST ${drawn})
            continue()
        endif()
        if(ENG_TEST_GPU_LOCK)
            set_property(TEST ${drawn} PROPERTY RESOURCE_GROUPS "gpu:1")
        endif()
        # A device taken away under it is a skip, beside whatever
        # else it already skips for.
        get_property(skips TEST ${drawn} PROPERTY SKIP_REGULAR_EXPRESSION)
        if(NOT skips MATCHES "device_taken_away")
            list(APPEND skips "${ENG_DEVICE_TAKEN_AWAY}")
            set_property(TEST ${drawn} PROPERTY SKIP_REGULAR_EXPRESSION "${skips}")
        endif()
        set_property(TEST ${drawn} APPEND PROPERTY ENVIRONMENT "ENG_LOG_KEYS=1")
        # Never less than the class's; one that asked for more keeps it.
        get_property(own TEST ${drawn} PROPERTY TIMEOUT)
        if(NOT own OR own LESS ENG_DEVICE_TEST_TIMEOUT)
            set_property(TEST ${drawn} PROPERTY TIMEOUT ${ENG_DEVICE_TEST_TIMEOUT})
        endif()
    endforeach()
endfunction()
