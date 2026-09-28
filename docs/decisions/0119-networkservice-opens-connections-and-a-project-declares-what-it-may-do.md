# 0119 — `NetworkService` opens connections, and a project declares what it may do

- Status: accepted (to be built; see `docs/briefs/network-kickoff.md`, C1 to C3)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27: *"eu não quero limitar o jogador a
  criatividade dele ... não gosto da ideia de bibliotecas nativas como net pode
  manter mas acho que deveríamos ter uma camada de abstração como serviço"*,
  and earlier the same day, that `NetworkService` should hold every way of
  connecting — HTTP, HTTPS, WebSocket, TCP and UDP.
- Builds on: [0063](0063-https-stays-refused-and-tls-comes-from-the-platform.md)
  (TLS from the platform), [0030](0030-std-convergence.md) (`@std/*` is the
  Lute-compatible surface).
- Amends: [0070](0070-a-port-is-opened-by-a-posture-and-never-by-a-script.md)
  (a port is opened by a posture and never by a script), already narrowed by
  [0106](0106-a-scene-is-a-place-and-the-game-changes-scenes-at-run-time.md)'s
  `NetworkService:Host`.

## Context

A script reaches the network through `@std/net.request`: a blocking-in-C++,
yielding-in-Luau HTTP/1.1 client, plain `http://` only (ADR 0063), named after
Lute rather than ADR 0034 so backend code runs on both runtimes. There is no
WebSocket, TCP or UDP for a script, although `engine/net` has a WebSocket
client (the dev channel's) and a TCP stream. A function in a module also has no
lifetime: a hot reload or a scene change leaves whatever it opened open.

## Decision

### 1. `@std/net` stays; `NetworkService` is the service over the same core

- Both call one C++ core in `engine/net`. `@std/net` keeps Lute's names and
  shape; `NetworkService` gives objects, PascalCase, and deferred signals.
- The match API (`Join`, `Host`, `Disconnect`, `State`, `Authority`, …) is
  unchanged; connections are new members beside it.

### 2. The members

- `NetworkService:Request(options): HttpResponse` — `Url`, `Method`,
  `Headers`, `Body` (string or `buffer`), `Timeout`; yields. `https://` works
  once A1 builds ADR 0063. `NetworkService:RequestAsync(options): HttpRequest`
  returns at once: `Progress`, `Completed`, `Cancel()`, streamed `Chunk`
  signals, and `Range` requests — what a video from a URL needs.
- `NetworkService:ConnectWebSocket(url, options?): WebSocket` — `ws://` and
  `wss://`; `Send(string | buffer)`, `Close(code?, reason?)`,
  `MessageReceived`, `Closed`, `State`.
- `NetworkService:ConnectTcp(host, port, options?): TcpSocket` — `Send`,
  `Receive` signal, `Close`, optional TLS (`options.Tls = true`).
- `NetworkService:OpenUdp(options?): UdpSocket` — `SendTo(host, port, data)`,
  `Received` signal (data, host, port), `Close`.
- `NetworkService:Listen(kind, port, options?)` — a `TcpListener` or a bound
  `UdpSocket`, **only when the project declares it** (§4).
- Binary data is a Luau `buffer` wherever bytes are.

### 3. A connection has an owner

- Every connection belongs to the script that opened it and to its scene. When
  the script is destroyed, its scene unloads, or a hot reload replaces it, the
  engine closes the connection and fires `Closed` with the reason. Nothing
  outlives the code that can hear it.
- Limits: `[net] max_connections` (default 64), `max_request_bytes`; exceeding
  one is a keyed error.

### 4. The project declares, the export shows

```toml
[permissions]
net_hosts = ["*"]    # hosts a game may reach; "*" (the default) is any
listen = false       # whether a script may open a listening socket
```

- **Outgoing connections are allowed to any host by default** — the owner's
  rule is not to limit what a game can do. `net_hosts` narrows it for an author
  who wants that.
- **Listening needs `listen = true`.** This amends ADR 0070: a script may open
  a listening socket, but only in a project that says so, and the Export window
  and `engine build` print it next to the target, as a phone's store listing
  prints a permission. `--host` and `NetworkService:Host` are unchanged.
- On a target without raw sockets (a browser, if the engine ever runs in one),
  `ConnectTcp`, `OpenUdp` and `Listen` fail with a keyed error that names the
  platform; `Request` and `ConnectWebSocket` keep working.

### 5. Seen in the editor

- A **Network** panel lists every request and socket of the play session:
  method or kind, address, status, bytes each way, timings, and a request's
  headers and body. F3 shows counts and bytes per second.

### 6. Determinism

As `@std/net` states: a response arrives when a server answers. Nothing here
enters the simulation unless a script writes it there, and the replay harness
records what arrived as input.

## Consequences

- A game can talk to anything: a leaderboard, a chat server, a custom protocol,
  a stream of video.
- A hot reload no longer leaks sockets.
- ADR 0070's invariant becomes "a port is opened by a posture, by
  `NetworkService:Host`, or by a script in a project that declares `listen`".
