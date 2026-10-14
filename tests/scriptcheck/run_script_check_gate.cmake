# A project `ludwerk check` calls clean is clean in the editor (ADR 0093).
#
# The editor's script pane and `ludwerk check` were two checkers with two
# configurations, and a game of a hundred files that passed the second opened
# in the first as 193 errors and 58 warnings: the pane's checker followed a
# require only through the tree, never by path; it ran Luau's new solver with
# every fix made since switched off; and the pane's own lint called a module
# required for its types unused, and four of the engine's globals unknown.
#
# `engine-host <project> --check-scripts` is the pane's checker with no pane,
# and this runs it three ways:
#
#   1. Over the project beside this file, which is that game's shape in five
#      files: nothing said.
#   2. Over every example, each of which the gate's own `ludwerk check` has
#      already called clean with the other analyser: nothing said.
#   3. Over a copy of the project with two mistakes written into it, where it
#      must say exactly those two, at their lines -- a checker that says
#      nothing about anything passes the first two.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/scriptcheck/project>
#         -DEXAMPLES=<examples> -DSCRATCH=<dir> -P run_script_check_gate.cmake

foreach(required HOST PROJECT EXAMPLES SCRATCH)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_script_check_gate.cmake: -D${required}=... is required")
    endif()
endforeach()

# Runs the checker; `problems` is every line that names a place in a script.
function(check_scripts project result_var problems_var output_var)
    execute_process(
        COMMAND "${HOST}" "${project}" --check-scripts
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    string(REGEX MATCHALL "[^\r\n]*\\([0-9]+,[0-9]+\\): (error|warning): [^\r\n]*" problems "${output}")
    set(${result_var} "${result}" PARENT_SCOPE)
    set(${problems_var} "${problems}" PARENT_SCOPE)
    set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

function(expect_clean project)
    check_scripts("${project}" result problems output)
    if(output MATCHES "scripts_no_checker")
        # A build with no editor has no checker to ask.
        message(STATUS "ENG_TEST_SKIP: this build has no script checker")
        return()
    endif()
    if(NOT result EQUAL 0 OR NOT "${problems}" STREQUAL "")
        string(REPLACE ";" "\n" listed "${problems}")
        message(FATAL_ERROR
            "The editor's checker reports problems in ${project}, which `ludwerk check` calls clean "
            "(exit ${result}):\n${listed}\n--- output ---\n${output}")
    endif()
    if(NOT output MATCHES "scripts_checked|Checked [0-9]+ script")
        message(FATAL_ERROR "The checker did not say it checked ${project}:\n${output}")
    endif()
endfunction()

# 1. The project by path.
expect_clean("${PROJECT}")
check_scripts("${PROJECT}" result problems output)
if(output MATCHES "scripts_no_checker")
    return()
endif()
if(NOT output MATCHES "Checked 5 script")
    message(FATAL_ERROR "The project has five scripts, and the checker did not check five:\n${output}")
endif()

# 2. Every example.
file(GLOB examples LIST_DIRECTORIES true "${EXAMPLES}/*")
set(checked 0)
foreach(example IN LISTS examples)
    if(EXISTS "${example}/project.toml")
        expect_clean("${example}")
        math(EXPR checked "${checked} + 1")
    endif()
endforeach()
if(checked LESS 20)
    message(FATAL_ERROR "Only ${checked} example projects were found under ${EXAMPLES}.")
endif()

# 3. Two mistakes, and exactly those.
file(REMOVE_RECURSE "${SCRATCH}")
file(COPY "${PROJECT}/" DESTINATION "${SCRATCH}")
file(WRITE "${SCRATCH}/src/client/Main.luau" "--!strict
local Match = require(\"../shared/Match\")
local Gone = require(\"../shared/Gone\")

local function round(context: Match.Context): number
    return context.Rounds
end

print(round(Match.make()), Gone)
")
check_scripts("${SCRATCH}" result problems output)
list(LENGTH problems count)
if(result EQUAL 0 OR NOT count EQUAL 2)
    string(REPLACE ";" "\n" listed "${problems}")
    message(FATAL_ERROR
        "Two mistakes were written into src/client/Main.luau and the checker said ${count} (exit ${result}):\n"
        "${listed}\n--- output ---\n${output}")
endif()
list(GET problems 0 first)
list(GET problems 1 second)
if(NOT first MATCHES "^src/client/Main\\.luau\\(3,[0-9]+\\): error: .*\\.\\./shared/Gone")
    message(FATAL_ERROR "A require of a file that is not there was not said at its line: ${first}")
endif()
if(NOT second MATCHES "^src/client/Main\\.luau\\(6,[0-9]+\\): error: .*Rounds")
    message(FATAL_ERROR "A field the required module's type does not have was not said at its line: ${second}")
endif()

message(STATUS "The editor's checker is clean on the project and on ${checked} examples, and says the two mistakes.")
