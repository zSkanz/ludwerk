--!strict
-- The condition this run measures. The gate's driver writes its own into each
-- run's copy of the project; this is the project's own, for a run by hand.
export type Condition = {
    Name: string,
    HangSeconds: number,
    -- A second player at the bot's machine, walked beside the first (ADR 0208).
    Guest: boolean,
    -- And the authority asked to put that guest's character a metre aside,
    -- three seconds into the walk.
    Nudge: boolean,
}

local Condition: Condition = {
    Name = "clean",
    HangSeconds = 0,
    Guest = false,
    Nudge = false,
}

return Condition
