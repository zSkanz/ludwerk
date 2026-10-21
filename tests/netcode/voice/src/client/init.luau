--!strict
-- Voice on, sent by its level rather than a key: the phrase is speech, so
-- the gate opens on it as it would on a player talking.
local VoiceChatService = game:GetService("VoiceChatService")

VoiceChatService.Enabled = true
VoiceChatService.PushToTalk = false
