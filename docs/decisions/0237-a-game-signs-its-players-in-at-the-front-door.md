# 0237 — A game signs its players in at the front door

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0220 (the manager and its front door), ADR 0224 (every
  match is encrypted), ADR 0226 (a ticket names the account), ADR 0238 (the
  data service)

## The question

ADR 0226 built the half of accounts a world needs: a server verifies a
signed ticket (a JWT, EdDSA) where it runs, asks the game's
`ValidateJoin`, keeps one session an account and bans by account. It left
the other half -- **who issues the ticket** -- to "the game's backend", and a
game that wants accounts has none. An online game made with the engine should
be able to sign its players in with nothing but its own server, and a game
that has a backend, or a platform, should keep using it.

## Decision

### 1. The manager signs players in, when the game asks it to

`[accounts]` in `servers.toml` turns it on:

```toml
[accounts]
enabled = true
register = true          # players may make an account; false: the console makes them
name_pattern = "^[A-Za-z0-9_]{3,20}$"
```

The front door (ADR 0220 §4) answers two more requests on the connection a
player already opens there, under the front door's own key (ADR 0224): **sign
in** and **register**. Neither is HTTP and neither needs a certificate: the
connection is the match handshake, Noise XX, with the front door's key pinned
as a server's is. A password never travels in the clear and is never stored:
it is kept as an Argon2id hash (`crypto_pwhash`, the libsodium the engine
already carries), with the parameters libsodium calls "moderate".

A sign-in that succeeds is answered with a **ticket**, signed with the key the
worlds already verify (`[network] ticket_public_key` is the manager's own
public key when `[accounts]` is on), `aud` the game's id, `sub` the account
id, 90 seconds, single use -- exactly what ADR 0226 verifies, so nothing in a
world changes. The client then joins as it does today, with that ticket.

### 2. On a client: `NetworkService:SignInAsync`

```luau
--!strict
local NetworkService = game:GetService("NetworkService")

local result = NetworkService:SignInAsync("realms.example.com", "ana", password)
if result.Ok then
    NetworkService:Join("realms.example.com", { Ticket = result.Ticket })
else
    print(result.Reason) -- Enum.SignInFailure: WrongPassword, NoSuchAccount, Taken, Refused, Unreachable
end
```

`RegisterAsync(address, name, password)` makes the account and signs it in.
Both yield and never raise for a refusal. A wrong password is answered after
the same work as a right one, and a name that does not exist after the same
work as one that does: neither tells an attacker which names are taken. Five
failed sign-ins from one address in a minute are refused for a minute.

### 3. Accounts live in the data service

An account is a row in the data service (ADR 0238): its id (a random 128-bit
value, never reused), its name, the hash, when it was made and last signed in,
and a ban (ADR 0226 §5) -- so the console's `ban` and an account's data are
one place. The console gains `account create|rename|password|delete`.

### 4. A backend or a platform keeps working

Everything above is off unless `[accounts]` says so. A game with its own
backend issues tickets with any JWT library, as ADR 0226 already allows. A
game on a platform (`IdentityService`, ADR 0188) gets the platform's token on
the client and its backend turns that into a ticket -- the engine does not
verify platform tokens itself, because each platform's verification is the
platform's and its keys are the developer's.

## Proof

- Register, sign in, join: a world's `ValidateJoin` sees the account id the
  manager issued, and `Player.AccountId` is it.
- A wrong password, a name that does not exist and a taken name are refused,
  and the time to answer each differs from a right one by less than 10%.
- Six failures in a minute: the sixth is refused without checking.
- The password is not in any packet capture of the exchange, nor in the data
  service's file.
- A ticket the manager issued is accepted by a world with no `[accounts]`
  of its own, and one from a stock JWT library still is.

## Consequences

- New API: `NetworkService:SignInAsync`, `RegisterAsync`,
  `Enum.SignInFailure`; `[accounts]` in `servers.toml`; console `account`
  commands. Protocol: two front-door messages, one bump.
- An online game has accounts with no backend of its own, and the realms
  example signs in.
- The manager holds the one key that signs tickets: losing it is losing
  every session to come; it is backed up with the front door's (ADR 0220).

## Not decided here

Recovering an account by e-mail (the engine sends no mail; a game's backend
does), two-factor sign-in, and linking a platform account to a manager one.
