--!strict
-- The condition this run measures, read by the authority and the bot alike.
-- The gate's driver writes its own into each run's copy of the project; this
-- is the project's own, for a run by hand.
export type Mode =
    -- A straight, a corner and a stop.
    "drive"
    -- A parked car twenty metres ahead, driven into.
    | "park"
    -- As `drive`, and the authority moves the car half a metre aside once in
    -- the straight: what a correction is, to show the measure sees one.
    | "nudge"
    -- Two players, each driving at the other with a wheel that wanders.
    | "duel"

export type Condition = {
    Name: string,
    Mode: Mode,
}

local Condition: Condition = {
    Name = "clean",
    Mode = "drive",
}

return Condition
