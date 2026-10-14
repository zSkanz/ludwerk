# 0226 — A server validates a joiner, and a ticket names the account

- Status: accepted; not built yet
- Date: 2026-10-14
- Builds on: ADR 0224 (a player is a key), ADR 0218 (bans), ADR 0220 (the
  front door, `Redirect`), ADR 0222 (the pass)

## The question

A key identifies a machine's copy of a game, not a person: delete the key
file and a banned player is a new one (ADR 0218). A game that has accounts --
a login on its own backend -- needs a server to know which account a joiner
is, without the server calling that backend for every join, and with a
player logging in once a session rather than once a world.

## Decision

### 1. `NetworkService.ValidateJoin`

```luau
NetworkService.ValidateJoin = function(joiner: JoinRequest): (boolean, string?)
    local claims = NetworkService:VerifyTicket(joiner.Ticket)
    if claims == nil then
        return false, "Please log in again."
    end
    joiner.AccountId = claims.sub
    return true
end
```

- **One callback, on the server**, as `OnServerInvoke` is: a second
  assignment replaces the first. A client that sets it is told it cannot.
- `JoinRequest` holds the joiner's `PublicKey`, `Address`, `Ticket` (a
  string, empty when none was presented) and `AccountId` (nil until the
  callback sets it).
- **It runs before the player exists**: no `Player`, no character, no
  `PlayerAdded`, nothing sent to the joiner but the wait. It may yield
  (to ask the game's backend over `net.request`); **after 10 seconds** it is a
  refusal (`ValidateJoin` timed out), and so is an error in it.
- Refused, the joiner is told the reason in `JoinFailed` -- the game's own
  words, at most 256 bytes -- with a new `Refused` reason (6,
  `RefusedByGame`), in the online server's one protocol bump.
- A server with no callback accepts every joiner, as today.

### 2. A ticket is presented once a session

- The client presents an opaque string: `NetworkService:Join(address,
  { Ticket = ticket })`. It travels inside the encrypted hello (ADR 0224),
  never in the clear.
- **Under a manager it travels on**: the front door keeps the ticket a player
  presented and puts it in each `Redirect` and pass (ADRs 0220, 0222), so
  every world the player reaches sees it in `JoinRequest.Ticket` and verifies
  it the same way. A player logs in once.

### 3. Verifying a signed ticket where the server is

`NetworkService:VerifyTicket(ticket)` checks a ticket the engine knows the
shape of, **locally, with no call out**, and returns its claims or nil:

- **Format: a standard JWT**, so any backend's ordinary JWT library issues
  one with no code of ours: a compact JWS, three base64url parts joined by
  dots, the header exactly `{"alg":"EdDSA","typ":"JWT"}`, the payload
  `{ "sub": "<account id>", "aud": "<game id>", "iat": <unix seconds>,
  "exp": <unix seconds>, "jti": "<unique id>" }`, signed with Ed25519.
- **The algorithm is not the ticket's to choose**: a header whose `alg` is
  anything but `EdDSA` -- `none`, `HS256`, `RS256`, or missing -- is refused
  before a byte of the signature is looked at, so no ticket can switch the
  check to one a forger can pass. The key is never taken from the ticket
  (`jwk`, `kid` and `x5u` are ignored).
- **Checked**: the signature against `[network] ticket_public_key` (the
  backend's Ed25519 public key, base64, in `project.toml`); `aud` equal to the
  project's `[project] id`; `exp` not past and at most **90 seconds** after
  `iat` (a ticket for longer is refused, so a backend cannot issue one that
  lives a day); `jti` not seen before by this server or, under a manager, by
  any of its worlds -- the manager keeps the ids until they would have
  expired. What is wrong is said in the server's log, never to the joiner.
- **Clocks**: `exp` and `iat` are compared with 30 seconds of tolerance each
  way, for a server whose clock is a little off.
- Under a manager, a world reached by a pass verifies the ticket's signature
  and audience again but not its expiry or its id: the pass (ADR 0222) is what
  proves the hop is fresh, and the ticket was already spent at the front
  door.

A backend issues one with the JWT library it already has; the manual's page
has these two:

```js
// Node, with jose
import { SignJWT, importPKCS8 } from "jose";
const key = await importPKCS8(process.env.TICKET_PRIVATE_KEY, "EdDSA");
const ticket = await new SignJWT({})
  .setProtectedHeader({ alg: "EdDSA", typ: "JWT" })
  .setSubject(account.id).setAudience("com.example.realms")
  .setIssuedAt().setExpirationTime("90s").setJti(crypto.randomUUID())
  .sign(key);
```

```python
# Python, with PyJWT and cryptography
import jwt, time, uuid
now = int(time.time())
ticket = jwt.encode(
    {"sub": account_id, "aud": "com.example.realms", "iat": now,
     "exp": now + 90, "jti": str(uuid.uuid4())},
    private_key, algorithm="EdDSA")
```

**Lute** has no signing at 1.0.0 (`@std/crypto` hashes, seals with a secret
box and hashes passwords; it does not sign). A backend written in Lute signs
with `ludwerk ticket sign --key=<file> --sub=<account> [--seconds=90]`, which
runs the engine's own code and prints a ticket, until Lute carries Ed25519;
the same command makes a key pair (`ludwerk ticket keygen`) and is what the
tests and the example's backend use.

A game with another scheme verifies its own in `ValidateJoin` and never calls
`VerifyTicket`.

### 4. One session per account

`Player.AccountId` is what `ValidateJoin` set (nil without it). **A second join
with an account that is already in** drops the first with the reason
`SignedInElsewhere`, across every world under a manager -- the manager knows
which account is where -- and lets the second in.

### 5. Bans by account

`ban` and `unban` (ADR 0218) take an account id where players have one, and
`bans.json` keeps it. A ban by account is checked after `ValidateJoin` has set
it and before the joiner is accepted -- on every world under the manager,
which shares one `bans.json`. This is the ban that holds: a new key with the
same account is the same banned account.

## Consequences

- A game with accounts knows who joined, once a session, without a call out
  per world; a banned account stays out.
- New API: `NetworkService.ValidateJoin`, `JoinRequest`,
  `NetworkService:VerifyTicket`, `Player.AccountId`, `Join`'s `Ticket`
  option; `[network] ticket_public_key`; `Refused` reason 6; the
  `SignedInElsewhere` removal; `ludwerk ticket keygen` and `sign`.
- Proof: a forged, expired, too-long, reused and wrong-audience ticket each
  refused, and so is one whose `alg` is `none` or `HS256`; one issued by a
  stock JWT library accepted; a second login drops the first; a banned account refused on every
  world under the manager.
- Manual: "Online games > Accounts and joining" -- `ValidateJoin`, the ticket
  (a JWT), issuing one from a backend.

## Not decided here

Accounts themselves, the data service and the example backend with a login
(a later record).
