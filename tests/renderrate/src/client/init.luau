--!strict

-- **The render half of the frame, headless** (G11). It was skipped when there
-- was no window: render-rate input, `BindToRenderStep` and `PreRender` -- so a
-- virtual stick injected into a `Render` context read nothing, and a render
-- step bound in a headless `--screenshot` never ran (K3).

local InputService = game:GetService("InputService")
local RunService = game:GetService("RunService")

local function actionAt(rate: EnumInputRate): InputAction
    local context = Instance.new("InputContext")
    context.Rate = rate
    context.Parent = workspace
    local action = Instance.new("InputAction")
    action.Type = Enum.InputActionType.Direction2D
    action.Parent = context
    local binding = Instance.new("InputBinding")
    binding.KeyCode = Enum.KeyCode.VirtualStick2
    binding.Parent = action
    return action
end

local render = actionAt(Enum.InputRate.Render)
local simulation = actionAt(Enum.InputRate.Simulation)
local steps = 0
RunService:BindToRenderStep("Probe", 1, function()
    steps += 1
end)
InputService:SetVirtualState(Enum.KeyCode.Virtual4, 1)
task.wait(0.5)
local fromRender = render:GetState() :: any
local fromSimulation = simulation:GetState() :: any
print(`[renderrate] render={fromRender.Y} simulation={fromSimulation.Y} steps={steps > 0}`)
