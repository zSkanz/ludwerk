--!strict
-- Each thread reads the value, adds one and writes it at the version it read;
-- where another thread wrote first, its function runs again.
local DataService = game:GetService("DataService")
local RunService = game:GetService("RunService")

-- From nothing, whatever an earlier run left: the value cleared, and the
-- writes counted from the version it is cleared at.
local _, before = DataService:GetAsync("event", "progress")
local _, base = DataService:SetAsync("event", "progress", nil, before)

local done = 0
for _ = 1, 3 do
    task.spawn(function()
        for _ = 1, 200 do
            DataService:UpdateAsync("event", "progress", function(old: any): any
                return ((old :: number?) or 0) + 1
            end)
        end
        done += 1
    end)
end

task.spawn(function()
    while done < 3 do
        RunService.PostSimulation:Wait()
    end
    local value, version = DataService:GetAsync("event", "progress")
    local stale = DataService:SetAsync("event", "progress", 0, base)
    print(`[shared] value={value} writes={version - base} stale={stale}`)
end)
