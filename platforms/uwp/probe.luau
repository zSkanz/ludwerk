--!strict
-- The platform probe loads this asset into the pinned, sandboxed VM. This is
-- deliberately not a substitute implementation of the engine's service API.
return function(x: number, y: number, ax: number, ay: number, reset: boolean, dt: number): (number, number)
    if reset then
        return 0.5, 0.5
    end
    return math.clamp(x + ax * dt * 0.5, 0.05, 0.95), math.clamp(y - ay * dt * 0.5, 0.3, 0.9)
end
