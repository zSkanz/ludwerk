--[[
	Ludwerk's match handler for Nakama (ADR 0242, as amended): a match that
	exists only to be named. When Nakama's matchmaker finds players for a
	game whose servers are Ludwerk's, `engine.lua` makes one of these, and its
	id is what the players' machines are told and what the game's servers ask
	Nakama about. Nobody joins it -- the game's own servers make the instance
	the match is played in -- and it ends at its first tick.
	Apache-2.0.
]]

local Handler = {}

function Handler.match_init(_context, _params)
    return {}, 1, "engine"
end

function Handler.match_join_attempt(_context, _dispatcher, _tick, state, _presence, _metadata)
    return state, false, "the game's servers make the place this match is played in"
end

function Handler.match_join(_context, _dispatcher, _tick, state, _presences)
    return state
end

function Handler.match_leave(_context, _dispatcher, _tick, state, _presences)
    return state
end

function Handler.match_loop(_context, _dispatcher, _tick, _state, _messages)
    return nil
end

function Handler.match_terminate(_context, _dispatcher, _tick, state, _grace)
    return state
end

function Handler.match_signal(_context, _dispatcher, _tick, state, _data)
    return state, ""
end

return Handler
