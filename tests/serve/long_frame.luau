--!strict
-- A long frame says where ITS time went (D540): one tick of this server stands
-- still for most of a second -- longer than a server catches up, which is half
-- of one -- and the warning about the frame has to put that time under the
-- simulation and its scripts. It read the frame before the long one, and said
-- the simulation had taken nothing. `long_frame_names_its_phases`
-- in `engine/app/CMakeLists.txt` runs it.
local RunService = game:GetService("RunService")

local ticks = 0
RunService.Heartbeat:Connect(function()
    ticks += 1
    if ticks == 40 then
        local deadline = os.clock() + 0.7
        while os.clock() < deadline do
        end
    elseif ticks == 80 then
        game:Shutdown()
    end
end)
