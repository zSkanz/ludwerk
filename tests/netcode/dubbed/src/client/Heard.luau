--!strict
--[[
	The joiner: it says nothing and plays nothing. It runs with its text and
	its voice in Brazilian Portuguese, and says what it was shown and what it
	heard of the two lines the server said -- each line's category, its
	length, how many ticks its caption stayed, whether a caption is wanted
	under `Auto`, and the words in the language it reads.

	Lines `[dubbed] ...`, which `run_dubbed.cmake` reads.
]]

local DialogueService = game:GetService("DialogueService")
local LocalizationService = game:GetService("LocalizationService")
local NetworkService = game:GetService("NetworkService")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

if NetworkService.Authority then
    return
end

print(
    `[dubbed] joiner text={LocalizationService.Locale} voice={LocalizationService.VoiceLocale} `
        .. `follows={LocalizationService.VoiceFollowsLocale} `
        .. `voices={table.concat(LocalizationService:GetVoiceLocales(), ",")}`
)

type Said = {
    Caption: Caption,
    From: number,
    Line: string,
}

local ticks = 0
local saying: { Said } = {}

DialogueService.CaptionStarted:Connect(function(caption: Caption)
    local sound = caption.Parent :: Sound
    -- Asked while it is being said: whether a caption is wanted depends on
    -- the file the line was found in, which is this machine's own.
    local line = `[dubbed] heard {caption.Text} category={sound.Category.Name} `
        .. `length={string.format("%.2f", sound.TimeLength)} `
        .. `shows={DialogueService:ShowsCaption(caption)} `
        .. `speaker={LocalizationService:Translate(caption.Speaker)} `
        .. `text={LocalizationService:Translate(caption.Text)}`
    table.insert(saying, { Caption = caption, From = ticks, Line = line })
end)

-- A caption is current for as long as it is in the list: asked every tick,
-- so the count does not hang on an event for an instance that is gone.
RunService.Heartbeat:Connect(function()
    ticks += 1
    local current = DialogueService:GetCaptions()
    for index = #saying, 1, -1 do
        local said = saying[index]
        if table.find(current, said.Caption) == nil then
            print(`{said.Line} ticks={ticks - said.From}`)
            table.remove(saying, index)
        end
    end
end)

local board = Workspace:WaitForChild("Board") :: Part
while board:GetAttribute("Done") ~= true or #saying > 0 do
    task.wait()
end
print(`[dubbed] server hello {board:GetAttribute("Hello")}`)
print(`[dubbed] server later {board:GetAttribute("Later")}`)
print("[dubbed] done")
-- Nothing more to hear: the run ends here and the server's with it.
game:Shutdown()
