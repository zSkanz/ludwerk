# 0070 — A port is opened by a posture and never by a script

- Status: accepted; NetworkService:Host(port) exists and is refused only on a dedicated server; decision 2 not met
- Date: 2026-08-27
- Narrows: [ADR 0035](0035-engine-is-a-websocket-client-of-the-dev-server.md)

## Context

ADR 0035 contains a sentence a dedicated server contradicts:

> **Only the dev server listens.** The engine opens no port in any profile, and
> the client half is compiled into dev builds only.

The first network work ships a dedicated server. It listens. The sentence and that work cannot
both stand as written, and the honest move is to say which part of the sentence
was the decision and which part was the circumstance that made the decision
cheap.

**The decision was the reason, and the reason survives intact.** ADR 0035 gives
it in its own Consequences: no port for the engine to allocate or collide on, no
firewall prompt on first run, and *no inbound parse surface reachable by anything
else on the box*. Those three are about what happens to somebody who runs a game.
None of them is about what happens to somebody who deliberately starts a server.

The failure this must keep preventing is specific and is worth naming rather than
gesturing at: **a game that opens a port because a script asked it to.** That is
the shape where a player double-clicks something from a game jam and acquires a
listening socket on their home network, where the firewall prompt is the first
they hear of it, and where the parse surface reachable from the outside is
whatever the author's protocol handler does with a hostile packet. A dedicated
server started with a flag has none of that shape: a person typed it, the machine
is one somebody chose to run a server on, and the surface is the engine's own
tested framing rather than a script's.

ADR 0041 solved the same *class* of problem for input and its rule is the one to
copy. Input reaches a script through the IAS's dispatch and **never from the OS
directly** — one path in, and the fact that there is only one is what makes the
guarantee checkable rather than a habit.

## Decision

**A listening socket is opened by a command-line posture and by nothing else.**

1. **The four postures are decided at process start, from arguments, and never
   change.** Run it and you are solo. Run it with `--host` and you are client and
   server at once. Run it with `--serve` (or `--serve=<port>`) and you are a
   dedicated server, which implies `--headless`. Run it with `--join=<address>`
   and you are a replica. Only the two that carry `--host` or `--serve` bind anything.

2. **No service, no property and no method can cause a bind.** `NetworkService`
   reports the posture; it cannot set it. `Authority`, `Topology` and
   `ServerTick` are read-only and `HostFact`, which is the same mechanism that
   keeps `DataModel.EngineVersion` out of the world hash — a fact about the
   build and the machine rather than about the world. The setters refuse by
   name rather than silently doing nothing. `NetworkService:Host(port)` does
   exist (`engine/script/src/services.cpp`): it queues a host request, which
   `engine/app/src/network_session.cpp` takes at the frame's safe point, so a
   script can open a listening socket. The only guard is refusal on a dedicated
   server (`refuseOnDedicated`). This clause is not met as first written, and
   the status line records that.

   This is the clause that makes the rule checkable: **grep the replication
   module for the bind call and its callers are in `app` only**: argument
   parsing, and the session's handling of a script's `Host` request. A rule
   whose enforcement is "nobody would do that" is not enforced.

3. **A build with no replication module cannot bind at all.**
   `ENG_ENABLE_REPLICATION` is `OFF` by default, and with it off the factory
   returns null, `NetworkService` reports solo, and there is no socket code in
   the binary to reach. `--host` in such a build is refused by keyed error at
   startup rather than ignored — a flag that silently does nothing is worse than
   one that is not there.

4. **The dev server's direction is untouched.** `ludwerk dev` still listens and the
   engine still dials out to it, and the client half is still compiled into dev
   builds only. Nothing about ADR 0035's actual mechanism changes; only the
   sentence generalising it does.

5. **A shipped game's launcher passes no posture flag.** ADR 0045's copied player
   binary is what a game ships to a person, and `--host` is not a flag a shipped
   game's launcher passes. The `player` preset compiles replication in, so a
   person who types `--host` into that binary gets clause 1's posture; nothing in
   the binary starts one on its own.

*Amended 2026-11-03 to match the code: a script can request a bind through
`NetworkService:Host(port)`, refused only on a dedicated server, and a dedicated
server is started with `--serve`, not `--headless --host`.*

## Consequences

The three properties ADR 0035 bought are kept for everyone who is not running a
server on purpose: a player's machine allocates no port, prompts no firewall and
exposes no inbound parse surface, in every profile that ships to a player, unless
a person starts it with `--host` or `--serve`.

What is given up is the *sentence*. "The engine opens no port in any profile" was
true and is now false, and the replacement is longer because it has to name the
one exception and fence it. That is the cost of a milestone the original ADR did
not have to think about; the alternative — a separate server binary — would
duplicate the whole engine to avoid rewriting a sentence, and it would break the
one-binary-four-postures property that makes `if NetworkService.Authority then`
a gameplay branch rather than a build flavour.

**A test can hold this.** The bind's callers are in `app` and a test can assert
that: that `NetworkService:Host` is refused on a dedicated server (the tests in
`engine/app/tests/network_session_tests.cpp` call it), that a replication-disabled
build refuses `--host` by key, and that the solo posture is what a world reports
with no arguments at all. Those are three assertions rather than a promise, which
is the difference between this ADR and a comment.

## Which profiles compile it in (recorded 2026-09-22)

The option's default stays `OFF`, and clause 3 holds for every profile that does
not set it. The presets set it in every profile except `shipping`
(`CMakePresets.json`):

- **`debug`, `dev` and the sanitizer builds**, because a module no gate compiles
  is a module nobody knows is broken. It had landed with the option off in every
  preset, so for its first commits no tier had built it or run its tests.
- **`editor`**, because that is what a person making a game runs, and a
  multiplayer game is tested by running it as a host and joining it — from the
  same download, on the same machine or a second one.
- **`player`**, which is why clause 5 no longer says the player binds nothing.
  *Amended 2026-11-03 to match the code.*

**`shipping` keeps it off**: a shipping build binds nothing and refuses `--host`
by key (clause 3).

## Alternatives considered

**Leave ADR 0035 alone and let the server contradict it.** Rejected: an invariant that
the code violates teaches everybody to read invariants as suggestions, and the
next person to want a port would have precedent rather than a rule.

**A separate `ludwerk-server` binary.** Rejected for the reason above and one more:
two binaries means two builds, two release artefacts, and a script that has to
know which one it is running in — which is precisely the configuration branch
that `roadmap.md`'s "gameplay branch, not a configuration branch" rules out.

**Let a script call `NetworkService:Host()`.** Rejected, and this is the one that
matters most. It is the ergonomic option and it is the exact shape of the failure
the original rule exists to prevent: a game that opens a port because its author
wrote a line, on a player's machine, with no flag anybody typed.
