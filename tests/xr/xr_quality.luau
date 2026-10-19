--!strict
-- A headset's quality (ADR 0250): `XRService` reads back the values the
-- session runs with -- a PC's defaults here, no project -- takes a script's
-- own within their ranges, and refuses one outside them, keeping the last.
-- Run under the simulated headset.
local RunService = game:GetService("RunService")
local XRService = game:GetService("XRService")

local function refused(set: () -> ()): boolean
    return not pcall(set)
end

local function said(): string
    return `msaa={XRService.Msaa} scale={XRService.RenderScale} resolution={XRService.ShadowResolution} `
        .. `distance={XRService.ShadowDistance} cascades={XRService.ShadowCascades} `
        .. `split={XRService.ShadowFirstSplit} effects={XRService.ScreenEffects} `
        .. `foveation={XRService.Foveation.Name} rate={XRService.RefreshRate}`
end

local ticks = 0
local first = ""
RunService.PostSimulation:Connect(function()
    ticks += 1
    if ticks == 5 then
        first = said()
        XRService.Msaa = 4
        XRService.RenderScale = 0.75
        XRService.ShadowResolution = 1024
        XRService.ShadowDistance = 30
        XRService.ShadowCascades = 3
        XRService.ShadowFirstSplit = 3
        XRService.ScreenEffects = true
        XRService.Foveation = Enum.XRFoveation.Medium
        XRService.RefreshRate = 90
        local refusals = {
            refused(function()
                XRService.Msaa = 3
            end),
            refused(function()
                XRService.RenderScale = 0.25
            end),
            refused(function()
                XRService.ShadowResolution = 4096
            end),
            refused(function()
                XRService.ShadowCascades = 5
            end),
            refused(function()
                XRService.RefreshRate = -1
            end),
        }
        local all = true
        for _, each in refusals do
            all = all and each
        end
        print(`[quality] first: {first}`)
        print(`[quality] set: {said()} refused={all}`)
    end
end)
