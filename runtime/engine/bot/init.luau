--!strict
--[[
	`@engine/bot` -- what a load test's bot can do (ADR 0236), the TYPE side.

	**This file is never run by a bot.** A bot process (`engine-host --bots`,
	which `ludwerk server loadtest --behavior=bots/name.luau` starts) runs the
	behavior in a VM of its own, where `require("@engine/bot")` is the bot
	itself, made in `engine/script/src/bot_behavior.cpp`. What this file is for
	is the checker: a behavior learns the bot's shape here.

	A bot runs none of the game: a behavior has the Luau libraries and this,
	and nothing else -- no `game`, no services, no other module. Every bot of a
	process runs the same file, each from its beginning, and again from it in
	each world the server sends it to.

	A game's script that requires this module gets the error below: the bot
	is not in a game.
]]

export type Bot = {
    --- Where the bot's character stood when its behavior began.
    Spawn: vector,
    --- Which bot of its process this is, from 0.
    Index: number,
    --- Walks to `position` on the ground's plane and **waits** until it gets
    --- there, within two metres, or `seconds` (30 unless given) go by. True
    --- when it got there.
    WalkTo: (self: Bot, position: vector, seconds: number?) -> boolean,
    --- **Waits** `seconds`; none, or 0, waits a tick.
    Wait: (self: Bot, seconds: number?) -> (),
    --- Where the bot's character is, or nil while it has none.
    GetPosition: (self: Bot) -> vector?,
    --- Where the parts carrying `tag` are, in the bot's copy of the world:
    --- what the server sent it.
    FindTagged: (self: Bot, tag: string) -> { vector },
    --- Writes a line to the bot process's log.
    Say: (self: Bot, text: string) -> (),
}

return (error("@engine/bot is a load test's bot; only a bot process (engine-host --bots) runs a behavior") :: any) :: Bot
