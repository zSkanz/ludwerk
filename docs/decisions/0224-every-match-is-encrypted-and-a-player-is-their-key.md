# 0224 — Every match is encrypted, and a player is their key

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds: ADR 0120 §1 to §3, which decided it and did not build it
- Builds on: ADR 0085 (a reconnect is the same player), ADR 0100 (the
  protocol is versioned), ADR 0178 (the relay forwards what it cannot read)

## The question

ENet sends what a match says in the clear and believes whoever says they are
who: anybody on the path reads it, and a player who learns another's token
is them. ADR 0120 decided the answer -- Noise over ENet with libsodium, and a
player is a key -- and the online server of this track cannot be offered
without it: bans (ADR 0218), passes (ADR 0222) and a player's saved data all
need an identity that cannot be borrowed.

## Decision

### 1. What is built

ADR 0120 §1 to §3 as written: libsodium vendored at its latest release tag;
Noise `IK_25519_ChaChaPoly_BLAKE2b` on the match's own socket before the
hello; then every ENet packet sealed with a per-direction key and a counter
nonce, a replayed or too-old one dropped; inside the ENet transport, so
nothing above `ITransport` changes. The relay (ADR 0178) forwards sealed
packets it cannot read, as it was built to.

### 2. On by default

- Every match is encrypted: a dedicated server, a listen host, a shipped
  game, a test.
- `--insecure` (or `[network] encrypt = false`) turns it off for a
  development run, says so in the log and the F3 overlay, and **a shipped
  player refuses it**. Both ends must agree; one that does not is refused with
  a reason that says which.

### 3. A server is a key, and how a player knows it

- Made on a server's first start, kept in its user data folder, its public
  key printed at start and by `status` (ADR 0218).
- **Under a manager** the player never has to know it: the front door's key
  is in the game's build (`[network] server_key`) or pinned on first contact,
  and every world after it is named, with its key, in a `Redirect` the
  previous world sent over an encrypted channel (ADR 0220, 0222).
- **A match joined by address or code** pins the host's key on first contact
  **without a word** -- a game on a LAN must not frighten a player -- and
  warns only if a pinned key ever changes: the warning says the host is not
  the one this machine met before, and what to do (ask the host; a host that
  reinstalled its game has a new key, and the pin is cleared from the game's
  settings).

### 4. A player is a key

- A key pair per game, kept in the player's user data folder.
- **`Player.PublicKey`** (a string) is the identity: the same next week on the
  same machine. `UserId` is derived from it, stable across a server's
  restarts, so a ban (ADR 0218) and a saved game hold. ADR 0085's token goes:
  the key does its job and is never sent.
- Several players at one screen (ADR 0205) are one key each, kept beside the
  machine's own.
- Moving a player to another machine is a game's decision: export the key, or
  bind it to the game's account (ADR 0226).

### 5. What it costs, measured and written

Before it lands: the cost a packet (sealing and opening) and a handshake,
on the load test of the MMO track (128 players, 60 Hz, one server core) and
on the Android emulator, with the numbers in this record and in the manual. The
expectation is well under a microsecond a packet; the record says what it
was.

### 6. Protocol

The handshake is before the hello, so the protocol version rises
(ADR 0100) -- **once for the online server**, with `Redirect` and the pass
(ADRs 0220, 0222). A build of one side cannot join the other.

## Consequences

- A match is private and its players are who they say they are.
- A player's identity survives a server's restart, which bans and saves need.
- `Player.PublicKey`; `[network] encrypt`, `[network] server_key`;
  `--insecure`.
- Manual: "Online games > Putting a game online" (encryption, keys, a pinned
  host) and `Player.PublicKey`'s page.

## Not decided here

Accounts: ADR 0226 lets a server know which one a joiner is, and a later record
keeps them. A player's key moved between machines.
