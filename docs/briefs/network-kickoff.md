# The network: the kickoff and the ledger

Block C of [`game-ready-plan.md`](game-ready-plan.md), approved by the owner on
2026-09-27. The order across blocks is in that file (C1 early, C4 later, C2 and
C3 last); this one is the order of work inside block C and where each piece
stands.

Decisions: [ADR 0119](../decisions/0119-networkservice-opens-connections-and-a-project-declares-what-it-may-do.md)
(C1 to C3), [ADR 0120](../decisions/0120-the-transport-is-encrypted-and-a-player-is-a-key.md)
(C4). **C1's `https://` waits for stage A1** of
[`foundation-kickoff.md`](foundation-kickoff.md).

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## Stage C1 — `Request` and WebSocket in `NetworkService` (ADR 0119 §1-3, §6)

- [ ] One C++ core in `engine/net` under both `@std/net` and `NetworkService`;
  `@std/net`'s names and behaviour unchanged (its conformance specs pass as
  they are).
- [ ] IDL: `NetworkService:Request`, `:RequestAsync` → `HttpRequest`
  (`Progress`, `Chunk`, `Completed`, `Cancel`, `Range`), `HttpResponse`.
- [ ] `NetworkService:ConnectWebSocket` → `WebSocket` (`Send`, `Close`,
  `MessageReceived`, `Closed`, `State`), `ws://` and `wss://`.
- [ ] `buffer` wherever bytes are.
- [ ] Ownership: a connection closes when its script is destroyed, its scene
  unloads, or a hot reload replaces it; `Closed` carries the reason.
- [ ] Limits `[net] max_connections`, `max_request_bytes`.
- [ ] Tests: a loopback HTTP server (streamed body, a range, a cancel); a
  loopback WebSocket echo; a hot reload closes an open socket; a scene change
  closes a scene's sockets and keeps a global script's.
- [ ] Docs: the networking guide opens with the service; `api-design.md`.

## Stage C4 — encryption, a player is a key, relay (ADR 0120)

- [ ] Vendor **libsodium** (manifest row, `vendor.luau`, `THIRD_PARTY_NOTICES.md`).
- [ ] Noise `IK` handshake over X25519, ChaCha20-Poly1305, BLAKE2; sealed ENet
  packets with per-direction keys and counter nonces; replay window.
- [ ] Inside `ITransport`; protocol version up; `docs/protocol/wire.md`
  regenerated.
- [ ] Server key pair in the user data folder; `Join(address, { ServerKey })`;
  trust on first use with a keyed warning on change.
- [ ] Client key pair per game; `Player.PublicKey`; `UserId` derived from it;
  ADR 0085's token retired.
- [ ] `--insecure` for development, visible in the log and F3; never in a
  shipped player.
- [ ] `engine relay`: a forwarding server for sealed packets; `[net] relay`;
  hole punching through its rendezvous, relay as fallback.
- [ ] Tests: a tampered packet is dropped; a replayed packet is dropped; a
  changed server key warns; a reconnect keeps `PublicKey` and `UserId`; two
  processes behind a simulated NAT meet through the relay (loopback).
- [ ] Docs: the multiplayer guide's *Playing over the internet*.

## Stage C2 — TCP, UDP and listening (ADR 0119 §2, §4)

- [ ] `ConnectTcp` (optional TLS), `OpenUdp`, `Listen` (TCP and UDP).
- [ ] `[permissions] net_hosts` (default any) and `listen` (default false),
  checked in the core; the Export window and `engine build` print them.
- [ ] ADR 0070's amendment note; the bind-callers check updated to its new
  sentence.
- [ ] A platform without raw sockets refuses by keyed error.
- [ ] Tests: `Listen` refused without the permission and accepted with it; a
  host outside `net_hosts` refused; a TCP and a UDP loopback round trip.

## Stage C3 — the Network panel

- [ ] Editor: a **Network** panel for the play session — every request and
  socket, address, status, bytes each way, timings; a request's headers and
  body.
- [ ] F3: connections, requests and bytes per second.

## Findings

(Filled as the work goes: what the ADRs assumed that reality corrected.)
