# 0219 — A server is administered from another machine, by key

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds on: ADR 0218 (the console), ADR 0120 (Noise `IK` and libsodium),
  ADR 0035 (the engine is a WebSocket client and never listens for its tools)
- Amends: ADR 0035's rule, for a dedicated server only, as §1 says

## The question

A server in a data centre has no terminal anybody sits at. Its operator wants
the console of ADR 0218 from their own machine: the same commands, the same
answers, and nothing a stranger who finds the port can do.

## Decision

### 1. Off unless asked, and on the machine's own address unless told

A server listens for administrators only when `[server] admin = true` or
`--admin` says so: on TCP, at `[server] admin_port` (the game's port plus one
by default), and **on 127.0.0.1 only** unless `admin_allow` names the
addresses it may also come from (`["203.0.113.4", "10.0.0.0/8"]`). A request
from anywhere else is closed before a byte of it is read.

ADR 0035 said the engine never listens, because a tool's port on a developer's
machine is reachable by every program on it. That rule stays for every
development posture. A dedicated server already listens -- it is what a
server is -- and this is a second port on it, off by default, authenticated
before anything is parsed.

### 2. An administrator is a key

- `ludwerk server keygen --name=ana` writes a key pair for an administrator
  (`ana.key`, private, and `ana.pub`), with libsodium (X25519).
- The server's `admins.toml` lists who may come in: a public key, a name and
  a level (ADR 0218 §3). Nobody is in it until somebody puts them there; a
  server with `admin = true` and an empty list says so at start and lets
  nobody in.
- The server's own key pair is ADR 0120's: made on first start, kept in its
  user data folder, its public key printed at start and by `status`.

**Who can read the files.** `admins.toml`, an administrator's `.key` and the
server's own key file are made readable by their owner only -- mode 0600 on
Linux, an access list with that user alone on Windows -- and the server
refuses to start an administered session from a key file anybody else can
read, saying which. The manual says to keep them out of version control, and
the project template's ignore list has them.

### 3. The channel is Noise `IK`, as a match's will be

The administrator knows the server's public key (`--server-key`, or pinned on
first contact with a warning if it ever changes, as ADR 0120 §2); the server
knows the administrator's from `admins.toml`. The handshake is Noise
`IK_25519_ChaChaPoly_BLAKE2b` -- the same primitives and the same code as the
match's (ADR 0224) -- and a key not in the list fails it. After it, each
message is a length and a sealed frame: a command line one way, the answer's
lines the other, and the log's lines if the administrator asked to follow it.

### 4. Bounded

- At most four administrators at once; a fifth is refused.
- A failed handshake from an address counts against it: five in a minute and
  that address is not answered for ten.
- A session with nothing for ten minutes is closed.
- A command line is at most 4 KiB, an answer is cut at 1 MiB with a note.

### 5. Said in the log

Every session's start and end, every command with the administrator's name
and key fingerprint (ADR 0218 §4), and every refused handshake with its
address.

### 6. The tool

```
ludwerk server connect play.example.com --key=ana.key [--server-key=...] [--follow]
```

The CLI runs the engine's own client for it (`engine-host --admin-connect`),
because the handshake and the sealing are the engine's code, not Lute's. A
prompt that sends each line and prints the answer; `--follow` also prints
the server's log as it is written; `--command="status"` runs one and exits
with its outcome, for scripts.

## Consequences

- A server can be administered from anywhere its operator allows, and from
  nowhere by default.
- One TCP listener in a dedicated server, which the shipping profile carries
  and the player profile does not.
- libsodium is vendored by ADR 0120's own approval; this is its second caller
  and its first to land.
- Manual: "Online games > Administering a server" -- turning it on, making
  an administrator, connecting, and where the keys must not go.

## Not decided here

A web panel. The manager's own console over several worlds (ADR 0220).
