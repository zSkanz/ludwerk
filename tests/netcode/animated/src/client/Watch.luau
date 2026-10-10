--!strict
--[[
	The joiner: it makes nothing and plays nothing. It waits for the
	character the server spawned, finds on it the animation player that came
	with it, and says what that player's graph did on THIS machine while the
	server walked, ran, jumped and attacked -- and what else the character
	arrived carrying.

	Lines `[animated] ...`, which `run_animated.cmake` reads.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

if NetworkService.Authority then
    return
end

local hero = Workspace:WaitForChild("Hero") :: CharacterBody
-- **The line the gate exists for**: before protocol 44 this never returned.
local animator = hero:WaitForChild("Animator") :: AnimationPlayer
print(`[animated] player graph={animator.Graph} retargeting={animator.Retargeting.Name}`)

local saw = {
    jump = false,
    fall = false,
    land = false,
    slash = false,
    back = false,
    step = false,
    hit = false,
}
local did: { string } = {}
animator.StateChanged:Connect(function(layer: string, from: string, to: string)
    -- From the walk on, as the server counts its own.
    local phase = hero:GetAttribute("Phase")
    if phase == nil or phase == "wait" or phase == "stand" then
        return
    end
    table.insert(did, `{layer}:{from}>{to}`)
    if layer == "Body" then
        saw.jump = saw.jump or to == "Jump"
        saw.fall = saw.fall or to == "Fall"
        saw.land = saw.land or (to == "Move" and from ~= "Move")
    elseif layer == "Arms" then
        saw.slash = saw.slash or to == "Slash"
        saw.back = saw.back or (from == "Slash" and to == "None")
    end
end)
animator.EventReached:Connect(function(name: string)
    saw.step = saw.step or name == "Step"
    saw.hit = saw.hit or name == "Hit"
end)

-- How fast the graph took the character to be going in each phase, once the
-- phase has had a second to settle: the mean of what it read.
local sums: { [string]: number } = {}
local counts: { [string]: number } = {}
local phaseNow = ""
local inPhase = 0
local sampling = RunService.Heartbeat:Connect(function(dt: number)
    local phase = hero:GetAttribute("Phase") :: string?
    if phase == nil then
        return
    end
    if phase ~= phaseNow then
        phaseNow = phase
        inPhase = 0
    end
    inPhase += dt
    local speed = animator:GetParameter("Speed")
    if inPhase > 1 and typeof(speed) == "number" then
        sums[phase] = (sums[phase] or 0) + speed
        counts[phase] = (counts[phase] or 0) + 1
    end
end)

local waited = 0
while hero:GetAttribute("Phase") ~= "done" and waited < 40 do
    task.wait(0.1)
    waited += 0.1
end
sampling:Disconnect()

local function mean(phase: string): string
    local count = counts[phase] or 0
    return if count == 0 then "none" else string.format("%.2f", sums[phase] / count)
end
print(`[animated] speed stand={mean("stand")} walk={mean("walk")} run={mean("run")}`)
print(
    `[animated] saw jump={saw.jump} fall={saw.fall} land={saw.land} slash={saw.slash} back={saw.back} `
        .. `step={saw.step} hit={saw.hit}`
)
print(`[animated] joiner did {table.concat(did, " ")}`)
print(`[animated] server did {hero:GetAttribute("Did") or ""}`)

-- And what else it arrived with, as the stamp authored it.
local body = hero:WaitForChild("Body") :: MeshPart
local cape = body:FindFirstChildOfClass("SpringBone") :: SpringBone?
local reach = body:FindFirstChildOfClass("IKControl") :: IKControl?
local feet = body:FindFirstChildOfClass("FootPlacement") :: FootPlacement?
local lamp = hero:FindFirstChildOfClass("PointLight") :: PointLight?
local hum = hero:FindFirstChildOfClass("Sound") :: Sound?
local streak = hero:FindFirstChildOfClass("Trail") :: Trail?
print(
    `[animated] carried cape={if cape then `{cape.RootJoint},{cape.Stiffness}` else "MISSING"} `
        .. `reach={if reach
            then `{reach.EndJoint},{if reach.Target then reach.Target.Name else "nil"},{reach.Weight},{reach.Smoothing}`
            else "MISSING"} `
        .. `feet={if feet then `{feet.LeftFoot},{feet.Hips},{feet.StepHeight}` else "MISSING"} `
        .. `lamp={if lamp then `{lamp.Range}` else "MISSING"} `
        .. `hum={if hum then `{hum.Looped},{hum.Volume}` else "MISSING"} `
        .. `streak={if streak then `{streak.Lifetime}` else "MISSING"}`
)
print("[animated] done")
-- Nothing more to see: the run ends here and the server's with it.
game:Shutdown()
