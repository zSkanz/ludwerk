# 0120 — The transport is encrypted, and a player is a key

- Status: accepted (to be built; see `docs/briefs/network-kickoff.md`, C4)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving the plan in
  `docs/briefs/game-ready-plan.md` and **approving the dependency libsodium**
  (R5), after a conversation comparing how other engines encrypt their game
  traffic.
- Amends: [0012](0012-networking-v1-primitives-only.md) (ENet behind
  `ITransport`, GameNetworkingSockets deferred) and
  [0085](0085-a-player-is-the-same-player-after-a-reconnect.md) (a player
  token keeps a `UserId`).

## Context

The match runs over ENet, which is UDP with reliability and nothing else: no
encryption, no authentication. `third_party/manifest.json` says so — "an ENet
channel is a LAN or a trusted link". Across the internet anyone on the path
can read and forge packets, and ADR 0085's player token, a bearer secret, is
sent in the clear.

How others do it: Valve's GameNetworkingSockets and Unreal each put their own
authenticated encryption (key exchange, then AES-GCM per packet) over UDP;
netcode.io does the same with libsodium; Godot and Unity use DTLS. DTLS needs a
certificate, which a match hosted from a player's machine does not have — in
practice it is run with verification off, which removes the protection.

A player hosting from home is also behind a NAT: nobody outside can reach them
without a relay or hole punching.

## Decision

### 1. Encryption over ENet, by a published handshake

- **libsodium** is vendored (ISC licence, pinned at the latest release tag by
  `vendor.luau resolve` when vendored, R5 satisfied by this record).
- The handshake is **Noise `IK`** (the pattern WireGuard uses), over X25519,
  ChaCha20-Poly1305 and BLAKE2 — a specification, not an invention. After it,
  every ENet packet is sealed with a per-direction key and a counter nonce;
  replays and reordering beyond ENet's window are dropped.
- The encryption lives inside the `ITransport` implementation; nothing above it
  changes. The protocol version rises (ADR 0100).
- A LAN or `--insecure` development posture may skip it, and says so in the log
  and the F3 overlay; a shipped player never does.

### 2. A server is a key

- A server has a long-term key pair, generated on first host and kept in the
  user data folder. Its public key is part of the address a client joins
  (`NetworkService:Join(address, { ServerKey = ... })`) or is pinned on first
  contact (trust on first use), with a keyed warning if it ever changes.

### 3. A player is a key

- A client has a long-term key pair per game, kept in its user data folder.
  **Its public key is the player's identity**: `Player.PublicKey` (a string),
  stable across reconnects and runs. `UserId` is derived from it, so ADR 0085's
  token is replaced by the key, which is never sent.
- `SaveService` on a server keys a player's data by it (ADR 0111).

### 4. Reaching a player behind a NAT

- **A relay**: `engine relay` — a small server program, shipped with the
  engine, that forwards sealed packets it cannot read between a host and its
  clients. A game points at a relay with `[net] relay = "host:port"`.
- **Hole punching** through the relay's rendezvous, with the relay as fallback.
- Both stay inside `ITransport`.

## Consequences

- A match can be played across the internet without anyone on the path reading
  or forging it.
- Identity without accounts: a player is the same player next week on the same
  machine. Moving a player between machines is a game's decision (export the key
  pair, or bind it to the game's own account).
- One more vendored library, small and widely audited.

## Not decided here

- GameNetworkingSockets. Its manifest row stays deferred; this record removes
  the reason it was wanted.
- Accounts, friends and matchmaking.
