# 0063 — `https://` stays refused, and when TLS arrives it comes from the platform

- Status: accepted
- Date: 2026-08-27
- Extends: [0012](0012-networking-v1-primitives-only.md) (networking is v1
  primitives only)
- Relates to: [0032](0032-binary-toolchain-artifacts-fetched-not-vendored.md) (binary
  toolchain artifacts are fetched, not vendored)

## Context

`net.request` speaks plain `http://` and refuses `https://` with a named error.
That is written down in the client's own header, in `docs/manual/guides/backend.md`
and in `docs/manual/concepts/sandbox.md`, and the refusal is the right one of the
three available behaviours — a silent downgrade would put a caller's credentials
in the clear on a line of code that reads as secure.

What was missing is a decision about the *other* side of it. The header says
"R5 forbids adding [a TLS library] without a human-approved ADR", which is true
and is not a plan: it names an obstacle without saying what the answer would look
like if somebody cleared it. So the next person to want `https://` starts the
whole design from nothing, and the most likely thing they do is vendor a crypto
library, because that is the obvious move.

This record exists to make the obvious move the considered one, and to say
plainly that it is not the recommended one.

## Decision

**`https://` stays refused in v1.** R15 closes v1's scope, ADR 0012 makes
`@std/net` primitives-only, and the documented shape — a backend on localhost or
a LAN — does not need it. Nothing here is blocked on it.

**When TLS is built, it comes from the PLATFORM and not from a vendored
library.** WinHTTP or Schannel on Windows, `NSURLSession` on macOS, the system
OpenSSL through the platform layer on Linux. The client becomes a thin seam over
whichever one the target has, exactly as `platform/` already is for files,
windows and clocks.

**Vendoring a TLS stack remains a human decision** and this record does not take
it. It is listed below with what it would cost, so that choosing it later is
choosing it.

## Why the platform, and not a vendored library

**A TLS library is a maintenance obligation, not a dependency.** Every other
vendored dependency in `third_party/` is pinned and left alone; that is what
ADR 0032 makes the manifest for. A TLS stack cannot be left alone. It is the
one dependency where a published CVE obliges a release, on somebody else's
schedule, and this project has no process for that and no reason to acquire one.
Shipping a pinned BoringSSL means shipping a version that will be out of date,
in a binary a player runs.

**The platform's stack is already patched.** Windows Update, macOS updates and
a distribution's package manager all keep the system TLS current for every
program on the machine. A game that uses it inherits that for nothing.

**The certificate store is the harder half and the platform has one.** A
vendored library gives no trust roots. Somebody would then have to choose
between shipping a CA bundle — which is a second thing that goes stale, and
which no engine should be in the business of curating — and reading the system
store per platform anyway, which is most of the work of just using the system
stack.

**R6 is satisfied either way and is not the constraint.** BoringSSL and mbedTLS
are both permissively licensed. This is not a licence decision; it is an
ownership one.

**The cost is that the client gets rewritten.** `engine/net/http.h` is a
hand-rolled HTTP/1.1 client over sockets — `Connection: close`, no keep-alive,
no redirects, chunked responses decoded. Layering platform TLS under it is not
possible on Windows or macOS, where the TLS and the HTTP are one API; the honest
version is that the client becomes a seam with three implementations and the
socket code goes. That is a real piece of work and it is the reason this is
recorded rather than done: it is out of v1's scope, and it is worth knowing that
the choice is "rewrite the client" rather than "add a library" *before* somebody
starts by adding a library.

## What the vendored route would cost, if it is chosen anyway

Recorded so the comparison is available rather than reconstructed:

- A pinned library in `third_party/` with a manifest row (ADR 0032), plus the
  build wiring for three platforms.
- A CA bundle or three per-platform trust-store readers.
- A standing obligation to watch that library's advisories and cut a release for
  a critical one.
- The same client rewrite is *not* required, which is the one genuine advantage:
  a vendored library layers under the existing socket code.

That last point is why the decision is not obvious, and why it is written down.

## Consequences

- `https://` keeps refusing, and the three documents that say so stay true.
- A future milestone that wants it has a starting point that is a seam rather
  than a shopping trip, and knows the client is what changes.
- The human-approval requirement the client's header names is preserved for the
  route that actually needs it: vendoring. The platform route needs no new
  dependency and therefore no approval under R5.

## Built (2026-09-27, the game-ready plan's A1)

- **One backend per platform, as decided**: WinHTTP on Windows (a session per
  process, whose connection pool is the keep-alive), `NSURLSession` on macOS
  (an ephemeral session), and the system's OpenSSL on Linux. `http://` stays on
  the socket client.
- **Linux loads OpenSSL at the first `https://` request** (`dlopen` of
  `libssl.so.3`, then `libssl.so`) rather than linking it: no OpenSSL is needed
  to build, a binary is not tied to one soname, and a machine without it fails
  that request by name (`net.err.https_no_openssl`). The socket client's HTTP
  runs over the TLS stream with `Connection: close`, so that path has no
  keep-alive -- the other three do.
- **Android, which this record predates, uses the Java platform**
  (`java.net.HttpURLConnection`, through JNI on the calling thread): the NDK
  offers no TLS, and the BoringSSL inside the system is a private library an
  app may not load. The Java stack verifies against the device's store,
  user-added roots included.
- **Redirects are `performHttp`'s, not the backends'**: every backend is told
  not to follow them, so one rule holds everywhere -- at most five, never
  `https` to `http`, `303` (and `301`/`302` after a `POST`) as a `GET`, no
  `Authorization` or `Cookie` to another host.
- **The tests make their certificate when they run**, never committed: Schannel
  serves it on Windows (a key under a name of its own, deleted after), the
  system OpenSSL on Linux. On Windows only the refusal can be tested -- adding
  a root asks the person at the machine -- and Linux tests the accepted request
  too, through a hook compiled only into a build with tests.
