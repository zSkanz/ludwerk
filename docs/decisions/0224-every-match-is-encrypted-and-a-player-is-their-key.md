# 0224 — Every match is encrypted, and a player is their key

- Status: accepted; built, amended where building it said otherwise (§1,
  §4 and "As built")
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

ADR 0120 §1 to §3, with libsodium (already vendored for `CryptoService`) for
the primitives: **Noise `XX_25519_ChaChaPoly_BLAKE2b`** on the match's own
connection before the hello; then every packet sealed with a per-direction
key and a counter nonce, a replayed or too-old one dropped. The relay (ADR
0178) forwards sealed packets it cannot read, as it was built to.

**XX, not IK** (amended). IK has the joiner know the host's key before the
first message, and a join by address or by code has no such key the first
time: the joiner would have to fall back to XX there anyway. XX is one
pattern for every join, sends both static keys encrypted, and costs one more
half round trip than IK, once, at the join.

**A transport around a transport** (amended), not a change inside the ENet
one: `createSecureTransport` wraps any `ITransport`, so the in-memory
transport every test runs on is encrypted as a player's is, and the relay's
path is too. What the layers above learn is two calls, `remoteKey(peer)` and
`localKey()`, and nothing is surfaced as connected until its handshake is
done.

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
- **`Player.PublicKey`** (a string, hexadecimal) is the identity: the same
  next week on the same machine, after the server restarts and on another
  server. A ban (ADR 0218), a saved game and a pass (ADR 0222) are kept by it,
  or by the account above it (ADR 0226).
- **`UserId` stays a number of this run of the server** (amended): given in
  join order, and the same again to a player who reconnects while the server
  runs -- now because the server knows the key, not because the player
  showed a token. Derived from the key, it would be a 32-bit number standing
  for a 256-bit one, two players could share it, and games that already treat
  it as "this match's player" would gain nothing. A server that restarts
  numbers everybody afresh, as before.
- **ADR 0085's token stays, inside the server** (amended): a key is given a
  token of this run the first time it joins, and a joiner who proved a key is
  that key's token whatever it sent. So a restarted server is still a fresh
  join to the player's machine -- what ADR 0085 built its "world replaced"
  rule on -- while a token alone no longer makes anybody anybody.
- **Several players at one screen** (ADR 0205) are keyed by the machine's key
  and their seat, hashed (amended): the same seat on the same machine is the
  same `PublicKey` again, with no second key to keep.
- Moving a player to another machine is a game's decision: export the key, or
  bind it to the game's account (ADR 0226).

### 5. What it costs, measured and written

Before it lands: the cost a packet (sealing and opening) and a handshake,
on the load test of the MMO track (128 players, 60 Hz, one server core) and
on the Android emulator, with the numbers in this record and in the manual. The
expectation is well under a microsecond a packet; the record says what it
was.

**Measured** (2026-10-14; the engine's `what a packet and a handshake cost`
test, and the load test run with and without `--insecure` on the same build):

| | 60-byte packet | 600-byte packet | A handshake, both sides |
|---|---|---|---|
| Desktop (Windows, x86-64, one core) | seal 0.31 µs, open 0.33 µs | seal 0.79 µs, open 0.79 µs | 0.37 to 0.43 ms |
| Android emulator (x86-64 image, virtualized) | 0.7 to 2.0 µs | 2.8 to 10 µs | 1.1 to 1.6 ms |

The emulator's numbers are an upper bound for an emulator, not a phone's: its
CPU is shared with the machine it runs on, and they moved by three times
between runs.

**The load test** (128 players as 32 machines of four, 1372 characters the
server walks itself, 150 m of interest, 60 Hz):

| | Encrypted | In the clear |
|---|---|---|
| Server tick, mean, for the whole world of 1500 characters | 15.42 ms | 15.15 ms |
| Sent, what the network carried | 1418 KB/s (11.1 KB/s a player) | 1290 KB/s (10.1 KB/s a player) |
| Received, what the network carried | 349 KB/s (2.7 KB/s a player) | 254 KB/s (2.0 KB/s a player) |

The two runs' worlds differ (the characters wander at random), so the
difference that is encryption's is read from what the network carried over
the messages inside it: **0.3 KB/s a player down and 0.7 KB/s up**, the
25 bytes of every packet, and **0.27 ms a tick** for every seal and open of
128 players. The 15 ms around it is the world's -- 128 players and 1372
characters walking, 1500 in all -- not what 128 players cost; the same
server with the players alone ticks in under a millisecond. The crowded run -- every player in everybody's
interest -- sends about 185 KB/s a player, where the 25 bytes are under 2 %
of a full packet; the 32 machines playing it could not keep up with that
much world on one computer and fell out, so only its first seconds are a
measurement, and they agree.

### 6. Protocol

The handshake is before the hello, so the protocol version rises
(ADR 0100) -- **once for the online server**, with `Redirect` and the pass
(ADRs 0220, 0222). A build of one side cannot join the other.

## As built

- **The handshake**: three messages (32, 96 and 64 bytes) reliable on the
  first channel, each tagged by its first byte; a sealed packet is a tag,
  an 8-byte counter and the ChaCha20-Poly1305 tag -- **25 bytes over the
  plain one**. The receiver keeps a window of 1024 counters. A handshake not
  done in ten seconds is let go. The prologue is `match <protocol> <[project]
  id>`: another game, or another build of this one, fails the handshake.
- **The joiner checks the host at the second message**, before it proves
  its own key: a host it has pinned with another key is refused as
  `KeyChanged`, and that host is told nothing.
- **Pins are by address only.** A join code names a match the relay found,
  and the relay hands the same code to another host later: pinned, it was a
  false `KeyChanged`. `NetworkService:ForgetServerKey(address)` lets a game
  trust a host again; `[network] server_key` names the game's own server's
  key, so that one is never taken on trust.
- **Where the keys are**: `keys/<saves folder>.key` beside the run's saves
  folder, so a player's, a dedicated server's (`saves-server`) and each window
  of a match tried from the editor are apart; the hosts it met in
  `keys/<saves folder>.hosts.json`. Mode 0600 where files have modes.
- **One copy of the game holds the key.** A lock beside the key file is held
  for the run; a second copy of the game on the same machine plays with a key
  of its own for that run. Given the same key, the server took it for the
  first player reconnecting and let the first one go.
- **Both ends must agree, and are told so.** An encrypted host that hears a
  hello in the clear answers, in the clear, a `Refused` with reason 6
  (`RefusedInsecure`), and the joiner reads "this server encrypts its
  matches". An encrypted joiner that hears anything but a handshake fails
  as `Insecure`.
- **A development run in the clear**: `--insecure`, `[network] encrypt =
  false`. A shipping build refuses the flag and ignores the setting, with a
  warning. The F3 overlay's network panel shows *Encryption: Off*.
- **What the socket carried** is now counted (`WireBytesSent`,
  `WireBytesReceived` in `GetStats`), so what encryption adds is measured
  rather than computed.
- **A defect it uncovered.** With the handshake's extra half round trip, a
  client's first answered intent could come one tick before the first tick
  it predicted. The replay after the first take stepped from the answered
  tick without the ticks between, so a character joining while still falling
  was replayed one gravity step short and corrected by 1.6 cm at the first
  comparison. Those ticks are now stepped standing, as a guest's missing
  ticks were (ADR 0208); the clean and lossy corner runs read 0 corrections.

## Consequences

- A match is private and its players are who they say they are.
- A player's identity survives a server's restart, which bans and saves need.
- `Player.PublicKey`; `NetworkService:ForgetServerKey`; `[network] encrypt`,
  `[network] server_key`; `--insecure`; `WireBytesSent` and
  `WireBytesReceived` in `GetStats`.
- Manual: "Online games > Putting a game online" (encryption, keys, a pinned
  host) and `Player.PublicKey`'s page.

## Not decided here

Accounts: ADR 0226 lets a server know which one a joiner is, and a later record
keeps them. A player's key moved between machines.
