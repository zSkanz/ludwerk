--!strict
--[[
	The lobby's rules (ADR 0105, ADR 0106): who is ready, and the move to the
	arena once everybody is.

	**Readiness is an attribute, and there is no `RemoteEvent` for it.** A
	player says "ready" with a key, which reaches the server as intent
	(`GetIntent`), and the server writes `player:SetAttribute("Ready", true)` --
	which every client sees, because attributes replicate. When every player
	is ready, the server loads the arena and every client goes with it.
]]

local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local SceneService = game:GetService("SceneService")

local leaving = false

RunService.Heartbeat:Connect(function()
    if leaving then
        return
    end
    local players = NetworkService:GetPlayers()
    local everyone = #players > 0
    for _, player in players do
        if player:GetIntent("Ready") == true and player:GetAttribute("Ready") ~= true then
            player:SetAttribute("Ready", true)
        end
        if player:GetAttribute("Ready") ~= true then
            everyone = false
        end
    end
    if everyone then
        leaving = true
        for _, player in players do
            player:SetAttribute("Ready", nil)
        end
        SceneService:LoadScene("scenes/arena.scene.json", { Round = 1, Players = #players })
    end
end)
