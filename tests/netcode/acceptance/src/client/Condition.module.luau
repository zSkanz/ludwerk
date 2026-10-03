--!strict
-- The condition this run measures. The gate's driver writes its own into each
-- run's copy of the project; this is the project's own, for a run by hand.
export type Condition = {
    Name: string,
    HangSeconds: number,
}

local Condition: Condition = {
    Name = "clean",
    HangSeconds = 0,
}

return Condition
