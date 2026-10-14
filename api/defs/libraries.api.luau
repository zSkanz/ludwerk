--!strict
--- Global libraries and free globals (api-design.md §1.1, §3.2).
---
--- `task` is the whole scheduling surface and there are no legacy globals --
--- no `wait`, no `spawn`, no `delay`, no `tick` (divergence #2, R8). The
--- removals are enforced in the VM and tested from C++; what is declared here is
--- what remains.
---
--- Luau's own libraries -- `math`, `table`, `string`, `coroutine`, `buffer`,
--- `bit32`, `utf8`, `debug`, `os`, `vector` -- are NOT declared here. They are
--- the language's, the analyzer already knows them, and re-declaring them would
--- make this file a second source of truth for something the pin already fixes.

local Schema = require("../schema")

local Libraries: { Schema.Library } = {
    {
        Name = "task",
        Records = { "ADR 0025" },
        Doc = "The scheduler: the functions a script uses to run code later, wait and start coroutines. Its "
            .. "timers count simulation time, not real time, so `task.wait(1)` is exactly 60 ticks at the default "
            .. "tick rate on every machine and in every run, and a replay reproduces it.",
        Functions = {
            {
                Name = "spawn",
                -- A generic pack, so `task.spawn(function(n: number) end, 1)`
                -- type-checks with the payload types intact. It has to be
                -- emitted UNNAMED -- `(fn: (T...) -> (), T...)` -- because a
                -- named type pack is a syntax error that takes the whole
                -- definitions file down with it, which is how this was found.
                Parameters = {
                    { Name = "fn", Type = "(T...) -> ()" },
                    { Name = "arguments", Type = "T..." },
                },
                Returns = { "thread" },
                ThreadSafety = "Unsafe",
                Doc = "Runs `fn` in a new coroutine immediately, before `spawn` returns, and returns that "
                    .. "coroutine. An error inside it does not reach the caller.",
            },
            {
                Name = "defer",
                Parameters = {
                    { Name = "fn", Type = "(T...) -> ()" },
                    { Name = "arguments", Type = "T..." },
                },
                Returns = { "thread" },
                ThreadSafety = "Unsafe",
                Doc = "Queues `fn` to run in a new coroutine at the next resumption point, in order with deferred "
                    .. "signal handlers. Returns the coroutine, which can be passed to `task.cancel`.",
            },
            {
                Name = "delay",
                Parameters = {
                    { Name = "duration", Type = "number" },
                    { Name = "fn", Type = "(T...) -> ()" },
                    { Name = "arguments", Type = "T..." },
                },
                Returns = { "thread" },
                ThreadSafety = "Unsafe",
                Doc = "Runs `fn` in a new coroutine after at least `duration` seconds of simulation time. A "
                    .. "duration of 0 or less means the next tick; the function never runs before `delay` returns.",
            },
            {
                Name = "wait",
                Parameters = { { Name = "duration", Type = "number?", Default = "nil" } },
                Returns = { "number" },
                Yields = true,
                ThreadSafety = "Unsafe",
                Doc = "Yields the current coroutine for at least `duration` seconds of simulation time, or until "
                    .. "the next tick if no duration is given. Returns the simulation time that passed, which is "
                    .. "a whole number of ticks.",
            },
            {
                Name = "cancel",
                Parameters = { { Name = "thread", Type = "thread" } },
                Returns = {},
                ThreadSafety = "Unsafe",
                Doc = "Cancels a coroutine that is scheduled to resume, such as one returned by `task.defer` that "
                    .. "has not run yet. Raises an error for a coroutine that has finished, has already been "
                    .. "canceled or is the one running.",
            },
        } :: { Schema.Method },
    },
}

local Globals: { Schema.Method } = {
    {
        Name = "warn",
        Parameters = { { Name = "values", Type = "...any" } },
        Returns = {},
        ThreadSafety = "Unsafe",
        Doc = "Prints its arguments like `print`, separated by tabs, as a warning. The message reaches "
            .. "`DebugService.MessageOut` at `Enum.LogLevel.Warning`.",
    },
}

local Definition: Schema.Definition = {
    Libraries = Libraries,
    Globals = Globals,
}

return Definition
