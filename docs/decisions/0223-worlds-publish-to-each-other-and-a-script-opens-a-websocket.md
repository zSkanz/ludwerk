# 0223 — Worlds publish to each other, and a script opens a WebSocket

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds on: ADR 0220 (the manager and its pipes), ADR 0035 (the engine's
  WebSocket client), ADR 0063 (TLS comes from the platform), api-design.md §7
  (`@std/net`)

## The question

A guild's chat, a world boss that appears for everyone, an announcement from
the operator: events that cross worlds. They are rare, small and not
movement -- movement never leaves the world it happens in. Separately, a game
talks to services of its own (a backend, a chat bridge, a leaderboard) that
speak WebSocket, and `@std/net` has an HTTP client and nothing else.

## Decision

### 1. Publish and subscribe, through the manager

```luau
local CrossWorld = game:GetService("CrossWorldService")
CrossWorld:SubscribeAsync("guild:42", function(message, origin) ... end)
CrossWorld:PublishAsync("guild:42", { from = player.Name, text = text })
```

- A topic is a string of at most 80 bytes; a message is what `SaveService`
  stores, at most 4 KiB once encoded.
- A world publishes on its pipe to the manager (ADR 0220 §2); the manager
  delivers to every world that subscribes, **the publisher included**, in the
  order it received them. `origin` is the publishing world's name.
- **Server side only.** Under a manager a message crosses worlds; a world
  on its own delivers it to itself, so a game's code runs the same tested on
  one machine.
- **At most once, not guaranteed**: a world that is restarting misses what
  was published meanwhile. What must not be lost is the data service's
  (a later record), not this.
- **Bounded**: a world may publish 200 messages a second and subscribe to
  1,000 topics; past that `PublishAsync` errors and says why. The manager
  keeps no history. Each world's inbound queue holds **1,000 messages or
  4 MiB**, whichever comes first; a world that falls behind loses the
  **oldest** first, counted in `GetStats().CrossWorldDropped` and said once
  a minute in its log. A message older than 10 seconds when its turn comes
  is dropped the same way: an event that late is not news.

Never movement, never a part: what crosses worlds is a game's event, and the
world it lands in decides what it does.

### 2. A WebSocket client for scripts

```luau
local net = require("@std/net")
local socket = net.websocket("wss://chat.example.com/ws", { Headers = {...} })
socket.Received:Connect(function(text: string) ... end)
socket:Send(text)
socket:Close()
```

- On the engine's existing client (ADR 0035), with TLS from the platform for
  `wss://` as `request` has it (ADR 0063) -- no new dependency.
- Text and binary (`buffer`) messages; `Closed(code, reason)`; connecting
  yields the calling thread, as `request` does.
- **Server side only by default**: a client's game that opens a socket is a
  game whose every player's machine talks to a service, which the project
  says it wants with `[permissions] websocket_client = true`. A shipped game
  without it refuses on a client, saying which key to set.
- Bounded: eight sockets a VM, 1 MiB a message.

## Consequences

- Cross-world chat, guild events and announcements need no service of the
  game's own.
- `@std/net` gains `websocket`, which category 8 (the web) needs anyway.
- New API: `CrossWorldService` (`PublishAsync`, `SubscribeAsync`,
  `Unsubscribe`), `net.websocket`.
- Manual: "Online games > Worlds and instances" (messages between worlds)
  and the `@std/net` page (WebSocket).

## Not decided here

Messages between worlds on different machines; a guaranteed queue (a later
record's data service).
