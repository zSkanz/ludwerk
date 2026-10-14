--!strict
-- An error in a thread of its own, so nothing catches it and this script goes
-- on to end the game as a player would.

task.spawn(function()
    error("the shop could not be opened")
end)

task.defer(function()
    game:Shutdown()
end)
