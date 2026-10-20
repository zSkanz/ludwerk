--!strict
-- The town, where the front door sends every player. Once the three have said
-- their names: a guild of them, Ana its founder, and Bob sent to the fields,
-- Cyd to the harbor. Once both have arrived, Ana is asked to say the guild's
-- line; once all three have heard it, Ana queues for a duel.
--
-- **The run after the kill** finds the traders the arena kept, and counts
-- what they hold instead: five swords and a thousand gold, or something was
-- duplicated or lost.
local DataService = game:GetService("DataService")
local MatchmakingService = game:GetService("MatchmakingService")
local NetworkService = game:GetService("NetworkService")

local guild = require("@engine/guild")

local EndToEnd = require("@shared/EndToEnd")

if NetworkService.WorldName == "" then
    return
end

MatchmakingService:DefineQueue("Duel", { Template = "arena", Teams = 2, TeamSize = 1 })

local here: { [string]: Player } = {}
EndToEnd.start(function(player: Player, name: string)
    here[name] = player
end)

task.spawn(function()
    local traders = EndToEnd.get("traders")
    if typeof(traders) == "table" then
        local swords = 0
        local gold = 0
        for _, owner in traders :: { string } do
            swords += #DataService:GetItemsAsync(owner)
            gold += DataService:GetBalanceAsync(owner, "Gold")
        end
        print(`[e2e] count swords={swords} gold={gold}`)
        return
    end

    EndToEnd.waitFor(function()
        return here.ana ~= nil and here.bob ~= nil and here.cyd ~= nil
    end)
    local ana, bob, cyd = here.ana, here.bob, here.cyd
    local id, why = guild.create(ana, "Wardens")
    if id == nil then
        print(`[e2e] town guild failed {why}`)
        return
    end
    for _, member in { bob, cyd } do
        guild.invite(id, ana, member)
        guild.join(id, member)
    end
    local members = 0
    local info = guild.get(id)
    if info then
        for _ in info.Members do
            members += 1
        end
    end
    print(`[e2e] town guild members={members}`)
    NetworkService:Travel(bob, "fields", { Name = "bob" })
    NetworkService:Travel(cyd, "harbor", { Name = "cyd" })

    EndToEnd.waitFor(function()
        return EndToEnd.count("arrived") >= 2
    end)
    ana:SetAttribute("Say", true)

    EndToEnd.waitFor(function()
        return EndToEnd.count("heard") >= 3
    end)
    local ticket, refused = MatchmakingService:QueueAsync(ana, "Duel")
    print(`[e2e] town queued ana={ticket ~= nil} {refused or ""}`)
end)
