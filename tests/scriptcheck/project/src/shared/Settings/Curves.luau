--!strict
-- A curve over a particle's life: the sequence types, which the editor's own
-- list of globals did not have.

local Curves = {}

function Curves.fade(): NumberSequence
    return NumberSequence.new({
        NumberSequenceKeypoint.new(0, 1),
        NumberSequenceKeypoint.new(1, 0),
    })
end

function Curves.ember(): ColorSequence
    return ColorSequence.new(Color3.new(1, 0.6, 0.2), Color3.new(0.2, 0, 0))
end

return Curves
