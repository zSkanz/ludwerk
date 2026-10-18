# 0239 — An online game's chat is an optional module, `@engine/chat`

- Status: accepted
- Date: 2026-10-17
- Supersedes: ADR 0132's withdrawal, for the scope below
- Builds on: ADR 0077 (`RemoteEvent`), ADR 0223 (`CrossWorldService`),
  ADR 0240 (parties and guilds), ADR 0241 (limits on every network call)

## The question

On 2026-10-03 the maintainer withdrew ADR 0132: "a game's chat is the game's own
code on `RemoteEvent` and the encrypted transport, and the engine ships
nothing chat-specific." On 2026-10-14 the maintainer approved the online-games plan, whose
item for its services reads: "Chat: local, party, guild, world, whisper; rate
limits; a filter hook; mute/ban from the console; short history on join; over
cross-server messages." The later word is the current one; the spirit of the
earlier -- nothing chat-specific in the engine's core -- is kept by building
it as Luau a game chooses to require, on general pieces the engine has for
every game.

## Decision

### 1. A module, not a service

`@engine/chat` is Luau, shipped beside `@engine/npclod`: a game that wants it
requires it on the server and on the client; a game that does not carries
nothing of it. No class, no property and no C++ in the engine is about chat.

```luau
--!strict
-- server
local chat = require("@engine/chat")
chat.start({
    channels = { "local", "party", "guild", "world", "whisper" },
    localRadius = 60,          -- metres
    history = 50,              -- lines a player is sent on joining a channel
    filter = function(text: string, from: Player): string? return text end, -- nil drops it
})

-- client
local chat = require("@engine/chat")
chat.messageReceived:Connect(function(message: chat.Message)
    print(`[{message.channel}] {message.fromName}: {message.text}`)
end)
chat.send("guild", "the gates are open")
chat.whisper("ana", "meet at the well")
```

### 2. The channels

- **local**: players whose characters are within `localRadius` of the
  speaker's, in this world.
- **party** and **guild**: the members of the speaker's party or guild (ADR
  0240), in whatever world, channel or instance each is, carried between
  worlds by `CrossWorldService` (ADR 0223) under a topic of the module's own.
- **world**: everyone in this world and its channels -- the worlds of one
  name under the manager.
- **whisper**: one player by name, wherever they are; "not online" when no
  world has them.

A message across worlds is at most once and arrives within the manager's
cross-world delivery (ADR 0223: 4 KiB, 200 a second a world); a chat line is
200 characters at most, so it fits.

### 3. Limits, filter, mute and ban

- **Rate**: five lines in five seconds a player by default, enforced on the
  server by the general limit every `RemoteEvent` now has (ADR 0241); a line
  past it is dropped and the sender told.
- **Filter**: the game's function, run on the server of the world the line
  was sent in, before it goes anywhere. The engine ships no word list.
- **Mute and ban**: read from the console's lists (ADR 0241 §4), which a
  script reads with `NetworkService:IsMuted(player)`; a muted player's lines
  are dropped where they are sent, a banned account never joins (ADR 0226).
- **History**: the last `history` lines of each channel a player is in are
  sent on joining a world, kept in the world's memory (world, local) or in the
  data service's shared values (party, guild; ADR 0238 §4).

### 4. What the engine gains, for every game

Two general pieces, neither about chat: a per-player rate limit on every
`RemoteEvent` and `RemoteFunction` (ADR 0241 §1), and a mute list kept by the
console and readable by a script (`NetworkService:IsMuted`, ADR 0241 §4).

## Proof

- Five channels between two worlds and an instance under one manager: a
  guild line sent in one world arrives in the other two, once.
- A whisper to a player in another world arrives; to one offline, "not
  online".
- The sixth line in five seconds is dropped and its sender told.
- The filter returning `nil` drops a line everywhere; a muted player's lines
  reach nobody.
- Joining a channel sends its last 50 lines.
- The realms example's chat is this module.

## Consequences

- An MMO maker gets the five channels a game of the kind is expected to have
  in a few lines; a game that wants another chat writes its own on the same
  pieces, as before.
- `@engine/chat` is code a game can read, copy and change.

## Not decided here

Voice chat (the audio category's work), and a UI: the module raises events and
the game draws them.
