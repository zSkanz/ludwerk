# 0012 — v1 networking = low-level primitives only (GNS + ENet behind ITransport)

- Status: accepted
- Date: 2026-08-19

## Context
User decision #6: v1 is single-player, but games must be able to open a
socket/web server and connect to one (developers build their own backends; an
official multiplayer solution comes later). Roblox's 2026 server-authority +
rollback release shows where replication eventually goes; retrofitting is the
failure mode to avoid.

## Decision
v1 ships **low-level primitives only**, exposed through the Lute-compatible
`@std/net` surface (HTTP client always; HTTP/WS server and raw sockets in dev
mode, and behind `[permissions] net_serve` in shipped builds). Native
transports: **GameNetworkingSockets** (encryption, congestion control,
console/mobile record) with **ENet** as the LAN/fallback option, both behind
the `ITransport` interface — the seam future replication will use (QUIC/msquic
can slot in later). There is **no NetworkService, no Remotes, no replication
in v1**; those names are reserved. The blessed v1 pattern for client/server
projects is a sibling backend process running on Lute sharing `@shared` code.

## Consequences
Honest scope; portable backend code (runs verbatim under Lute); the simulation
core (ADR 0016) plus `ITransport` keep the path to official multiplayer open
without v1 paying for it.

## Amended, 2026-10-02 (the netcode audit)

- **The transport is serviced on a thread of its own.** Acknowledgements and
  pings leave the moment a packet arrives, whatever the frame loop is doing; a
  window ten frames a second in the background read a ping of 100 ms before.
  What arrives waits for `poll`, which the frame calls where it always did, so
  nothing reaches the simulation at any other point. A frozen game no longer
  looks silent to the transport, so the sessions notice silence themselves:
  ten seconds without a message and the peer is gone.
- **ENet is configured, not left at its defaults**: the packet throttle never
  throttles (it dropped snapshots and intents for seconds after any spike), the
  datagram is 1200 bytes (a VPN's path is smaller than ENet's 1392), and on
  Windows a vanished peer's "port unreachable" no longer cuts a receive short.
  A disconnect goes after what was queued to the peer.
- **A link conditioner below the transport** (`--net-delay`, `--net-jitter`,
  `--net-loss`, and `TransportConfig::simulated*`): a relay in the process,
  so ENet's own round trip and resends see the conditions. Refused by a
  shipping build.
