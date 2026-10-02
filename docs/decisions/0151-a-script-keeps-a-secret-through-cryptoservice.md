# 0151 — A script keeps a secret through `CryptoService`

- Status: accepted (built, 2026-10-02)
- Date: 2026-10-02
- Decided by: ludwerk-08 for the owner, from the first multiplayer game made
  with the engine (finding M7, defect D442): *"A small crypto surface: random
  bytes and a unique id from the operating system's generator, SHA-256 and
  HMAC, and a password hash meant for passwords. Transport encryption is
  already in the game-ready plan; a login is the reason it cannot stay at the
  end of it."*
- Amends: [0120](0120-the-transport-is-encrypted-and-a-player-is-a-key.md)
  (libsodium, approved there, is vendored now and for a second caller; where
  its transport stands in the plan).

## Context

A script's only chance was the simulation's. `Random.new()` and `math.random`
give the same numbers on every launch, which is the point of them (R10): a
replay and a second machine must agree. A session token, a salt and an identity
key need the opposite, and there was nothing else to ask -- so the game seeded
a generator from `os.time()` and `os.clock()`, which is a guess an attacker can
make too, and carried its own SHA-256 written in Luau. It had nothing meant for
a password at all, and a plain SHA-256 of one is guessed at billions of tries a
second.

How others do it: Godot ships `Crypto` and `HashingContext` over mbedTLS;
Unity and Unreal hand a game the platform's or OpenSSL's; every one of them
gives a script the operating system's generator and none asks a game to write
a hash.

## Decision

### 1. One service, eight calls

`CryptoService`, on every side:

| Call | Is |
|---|---|
| `RandomBytes(count)` | a buffer from the operating system's generator |
| `RandomInteger(min, max)` | a whole number in the range, every one as likely as the next |
| `UniqueId()` | a version-4 UUID, lower case: 122 random bits |
| `Sha256(data)` | SHA-256, as 64 hexadecimal characters |
| `HmacSha256(key, data)` | HMAC-SHA-256 (RFC 2104), the same way |
| `SecureEquals(a, b)` | equality in a time that does not depend on where they differ |
| `HashPasswordAsync(password)` | Argon2id, as a line of text that carries its salt and cost |
| `VerifyPasswordAsync(password, hash)` | whether the password is the hash's |

`data`, `key`, `a` and `b` are a string or a buffer: what is hashed is bytes.
A digest is hexadecimal text because that is how every service a game talks to
spells one, and a script that wants the bytes has `buffer`.

**Small on purpose.** No symmetric encryption, no signatures, no key exchange:
each is a way to build a protocol, and a game that builds its own protocol is
the thing ADR 0120 exists to make unnecessary. They can be added when a game
shows the need; a call cannot be taken back.

### 2. The primitives are libsodium's

Nothing here is written here. ADR 0120 approved libsodium for the transport;
it is vendored now (1.0.22, the latest release tag) and `engine/core/crypto.h`
is the only header that includes it. One library for both callers rather than
a hand-written SHA-256 today and libsodium later.

It is built by a target of ours from its sources, with none of the `HAVE_*`
macros its configure script would define, so every primitive compiles its
reference implementation: the same code on all four tiers, no per-file
compiler flags, no assembler. Nothing a game does with it is per-frame.

### 3. A password is hashed off the tick, one at a time

Argon2id at libsodium's "interactive" cost -- two passes over 64 MiB, tens of
milliseconds -- is several ticks of a server, so `HashPasswordAsync` and
`VerifyPasswordAsync` yield, and the work is done on a thread the first call
starts. **One thread and a queue**, not a thread a request: a hundred logins
arriving together are hashed one after another in 64 MiB, where a thread each
would be 6 GiB and a way to take a server down by knocking. A password is at
most 1024 bytes for the same reason.

The hash is text that names its algorithm, cost and salt
(`$argon2id$v=19$m=65536,t=2,p=1$...`), so the cost can be raised one day and
every stored hash still verifies.

### 4. It is not the simulation's

What the service answers is a fact about this machine at this moment (the
`SaveService` rule, ADR 0111): a replay does not reproduce it and a client
does not predict it. It belongs where a server decides, or outside the
simulation. The world hash never sees it.

### 5. The transport moves up the plan

A login is why. Until ADR 0120 is built a password sent through a
`RemoteEvent` travels readable, and the manual says so beside the calls and
points at `https://`, which is encrypted today. ADR 0120's first slice -- the
handshake, sealed packets, the server's key and the player's -- follows the
gesture work in the queue instead of standing at the end of the game-ready
plan; its relay and hole punching stay where they were.

## Consequences

- A game can make a token, sign one, and store a password properly, with
  nothing of its own to get wrong.
- One more vendored library, 2.6 MiB of source, small and widely audited.
- `Sha256` will be used for passwords by somebody anyway. Its documentation
  says not to, in the first paragraph.

## Not decided here

- Encryption and signatures for a script.
- Accounts. A game still brings its own, or a backend.
