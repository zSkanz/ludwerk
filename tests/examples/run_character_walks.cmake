# `examples/37-character`'s walker walks at the speed it is asked for (D615),
# and the example boots clean.
#
# The walker is a hero as a game makes one: a capsule, its body welded inside
# it, a staff welded to a bone on its hand, and a script that turns it each
# tick to face the way it walks. Its body and its staff collide with nothing
# -- and were taken for ground: a character inherits its ground's motion, and
# its own body's motion is its own a tick ago, so a walk of 1.1 metres a
# second fed on itself. On the build before, 376 of 600 ticks were faster than
# 1.3 m/s and the fastest was 9.3; a stair's step-up is the only tick that is,
# five or six times in a round.
#
# A copy of the example, with a script added that measures the walker each
# tick and says what its animation graph is doing; then:
#
#   - no warning and no error in the run;
#   - at most a dozen ticks over 1.3 m/s, and none over 3;
#   - the walker's graph is in `Move`, reading a speed near its walk, and its
#     clips' `Step` events arrive.
#
# No device: what is under test is the simulation.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<examples/37-character> -DSTAGE=<dir>
#         -P run_character_walks.cmake

foreach(required HOST PROJECT STAGE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_character_walks.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE PATTERN "tools" EXCLUDE)
file(WRITE "${game}/src/server/zz_measure.luau" [=[
--!strict
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local walker = Workspace:WaitForChild("Stocky") :: CharacterBody
local animator = walker:WaitForChild("Animation") :: AnimationPlayer
local steps = 0
animator.EventReached:Connect(function(name: string)
    if name == "Step" then
        steps += 1
    end
end)

local last = walker.Position
local ticks, fast, fastest, speedRead, inMove = 0, 0, 0, 0, 0
RunService.Heartbeat:Connect(function(dt: number)
    ticks += 1
    local now = walker.Position
    local dx, dz = now.x - last.x, now.z - last.z
    local speed = math.sqrt(dx * dx + dz * dz) / dt
    last = now
    if ticks > 30 and ticks <= 630 then
        if speed > 1.3 then
            fast += 1
        end
        fastest = math.max(fastest, speed)
        if animator:GetState() == "Move" then
            inMove += 1
        end
        speedRead += (animator:GetParameter("Speed") :: number)
    end
    if ticks == 631 then
        print(
            `[walks] fast={fast} fastest={string.format("%.2f", fastest)} move={inMove} `
                .. `speed={string.format("%.2f", speedRead / 600)} steps={steps} walk={string.format("%.2f", walker.WalkSpeed)}`
        )
    end
end)
]=])

execute_process(
    COMMAND "${HOST}" "${game}" --headless --rhi=null --frames=700 --exit
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message("${output}")
    message(FATAL_ERROR "character walks: the host failed (${result})")
endif()
if(output MATCHES "\\[error\\]" OR output MATCHES "\\[warn\\]")
    message("${output}")
    message(FATAL_ERROR "character walks: the example warned or failed while it ran")
endif()

string(REGEX MATCH "\\[walks\\] [^\n]*" line "${output}")
if(line STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "character walks: the walker was not measured")
endif()
message("${line}")
string(REGEX MATCH "fast=([0-9]+)" _ "${line}")
set(fast "${CMAKE_MATCH_1}")
string(REGEX MATCH "fastest=([0-9.]+)" _ "${line}")
set(fastest "${CMAKE_MATCH_1}")
string(REGEX MATCH "move=([0-9]+)" _ "${line}")
set(move "${CMAKE_MATCH_1}")
string(REGEX MATCH " speed=([0-9.]+)" _ "${line}")
set(speed "${CMAKE_MATCH_1}")
string(REGEX MATCH "steps=([0-9]+)" _ "${line}")
set(steps "${CMAKE_MATCH_1}")

set(failures "")
if(fast GREATER 12)
    string(APPEND failures "  ${fast} of 600 ticks faster than 1.3 m/s, where a walk of 1.1 has a stair's step-up or six\n")
endif()
if(fastest GREATER 3)
    string(APPEND failures "  the fastest tick was ${fastest} m/s\n")
endif()
if(move LESS 590)
    string(APPEND failures "  its graph was in Move for ${move} of 600 ticks\n")
endif()
if(speed LESS 0.9 OR speed GREATER 1.3)
    string(APPEND failures "  its graph read a speed of ${speed} m/s on average, of a walk of 1.1\n")
endif()
if(steps LESS 8)
    string(APPEND failures "  ${steps} steps were heard in ten seconds of walking\n")
endif()
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "character walks: the walker did not walk as it was asked:\n${failures}")
endif()
message("character walks: 600 ticks at its own speed, in Move, its steps heard")
