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

## Amendment (2026-10-19): the hashes, the reasons, and the account at the door

### A. The hashes: "interactive", in a bounded pool

§1 named libsodium's "moderate" parameters: 256 MiB and three passes for
every hash. At that, two hashes at once are a few sign-ins a second, and a
server opening its doors to a few hundred players turns into a line that
mostly hears `Refused`. The recommendation for a server's password hashes
(OWASP's for Argon2id) is far smaller: 19 to 64 MiB, one to three passes.
**The manager hashes with libsodium's "interactive"** -- 64 MiB, two passes,
what `CryptoService:HashPasswordAsync` already uses -- which is at the top of
that range.

**The hashes run in a pool of their own**, min(cores / 2, 4) threads, with at
most 64 waiting; the manager's loop never waits on one. A sign-in that finds
the pool full is answered at once with `Busy` and the seconds to try again
after. The pool's memory is what its threads hash at once: 4 × 64 MiB at most.

**A sign-in that names no account does a hash's work all the same**, against
a hash made when the manager starts, so the time to answer does not say
which names are taken.

**Measured**, as part of this ADR's proof: at least 20 sign-ins a second on
a machine of four cores, with the manager's peak memory recorded beside it.

### B. The reasons

§2's `Enum.SignInFailure` named `WrongPassword` and `NoSuchAccount` apart,
which would tell an attacker in words what §2 takes care to hide in the time
it answers. The reasons are:

- `WrongNameOrPassword`: the name has no account, or the password is not
  its own. One reason for both.
- `Taken`: `RegisterAsync` with a name an account already has. A register
  says so; it is how a player picks another name.
- `InvalidName`: not what `[accounts] name_pattern` allows.
- `InvalidPassword`: under 8 bytes or over 128.
- `Busy`: the pool was full; `Result.RetryAfter` says when to try again.
- `Refused`: the game's manager signs nobody in (`[accounts]` is off, or
  `register = false` for a register), or five failures from this address in
  the last minute.
- `Unreachable`: no answer from the front door.

### C. The account at the door, and on every pass after it

§1 left a ticket to be checked in each world, by the game's `OnJoinRequest`.
Under a manager that is the wrong place for it: a ticket lives 90 seconds
and is used once, so a player who travels after that arrives with a ticket
the next world must refuse -- and a pass of a travel carried no ticket at
all, so a game that checks tickets refused every traveller, and one that
did not had `Player.AccountId` empty after the first hop, its records then
kept under the player's key.

So, with `[accounts]` on, **the front door checks a hello's ticket itself** --
its own signature, the game's audience, the 90 seconds and the single use --
and refuses at the door a joiner whose ticket does not hold. **The pass it
gives names the account**, and so does every pass of a travel or an update
after it, the manager knowing who each world holds. A world takes the
account from the pass: `Player.AccountId` is it, a ban by account and one
session an account apply as they do for an account a game named, and a
game's `OnJoinRequest`, where it has one, finds `joiner.AccountId` already
set. A world needs no `ticket_public_key` of its own. **The account is
inside what the manager signs** (ADR 0222's pass): a pass reaches a world
through the player's machine, and one whose account was changed on the way
is refused as one the manager never signed.

Without `[accounts]` the passes carry the account all the same: the one a
game named in its entry world's `OnJoinRequest`, so its other worlds know it
without asking again.

### D. Bans

§3 put an account's ban in its row. Bans stay in `bans.json`, under
`account:<id>`, which every world of the game already reads: one list for
bans of keys and of accounts, and nothing in the database to keep in step
with it. Deleting an account does not lift its ban.

### E. The protocol

Two front-door messages, both on the control channel: `SignIn` (to the
door: a name, a password, and whether to register) and `SignedIn` (to the
player's machine: a ticket, or a reason and when to try again). The
connection is the one a join opens, Noise XX with the front door's key
pinned, so the password crosses the network only encrypted.

### F. Login names

A login name is **ASCII alone**: three to twenty of `A-Z`, `a-z`, `0-9`,
`_`, `.` and `-`, and a game's `name_pattern` may only narrow it. Unicode
names would need Unicode's normalization tables, a dependency of their own,
for what a game's own display names already give a player. **Two names are
one account where they look alike**: they are compared by a skeleton -- the
letters' case folded, and `l`, `I` and `1` one character, `o` and `0`
another -- so `Admin`, `admin` and `Adm1n` cannot be three accounts, and a
register of the second answers `Taken`. A sign-in finds a name whatever its
case. What a game shows a player as is the game's.

### G. Many addresses against one account

Five failures from one address in a minute are refused for a minute (§2);
an attacker on many addresses is not. So each name -- by its skeleton, and
whether or not an account has it, so the delay says nothing either -- counts
its own failures: after five in fifteen minutes, each attempt on it waits
before its hash, 1 second, then 2, 4 and so on to 32, in the manager's queue
and never in its loop. Nothing locks an account: a right password clears its
count, and so do fifteen minutes without a failure, so an attacker can slow
the account's own player and never shut them out.

## As built (2026-10-19)

- **The front door** answers `SignIn` and `SignedIn` (protocol 61) on the
  connection a join opens, under its own key (ADR 0224); the player's
  machine opens one for each `SignInAsync` or `RegisterAsync`, pinned as a
  join's host is, and lets it go once answered or after a minute. The
  password is wiped from the message once it has left, and from the door's
  memory once hashed or checked.
- **Accounts** are the data store's, schema version 5: an id of 128 random
  bits, the login name, its skeleton (unique), the Argon2id hash, when it was
  made and last signed in. Only the asker's thread reads and writes them;
  the pool's threads see a password and a hash, never the database.
- **The hashes** (amendment A): `crypto_pwhash` at "interactive", in a pool
  of min(cores / 2, 4) threads with 64 waiting at most; past that, `Busy`
  and a second. A name with no account is checked against a hash made when
  the manager starts. `accounts_timed`, a timed test run beside the gate's
  other timed tests, holds a wrong password, a name with no account and a
  taken name within a tenth of a right one's time -- measured a round of
  all four at a time, so the machine's drift falls on each alike -- and
  signs 200 in on a pool of four at half again one thread's rate or more,
  one thread's measured beside it on the same machine -- a floor in sign-ins
  a second failed on a shared runner, at 19.8 against 20, where four cores
  held three other tests too. Each hash takes 64 MB, so four share the
  memory's bandwidth: quiet, the pool makes about twice one thread's. **Measured**
  on a machine of twenty threads, quiet, three runs: each kind's median 46.5
  to 47.1 ms, within 1.3% of a right one's; 44.5 to 48.2 sign-ins a second
  on four threads, the process's resident memory 14 MB before and 271 MB at
  its most. A hash is about 47 ms on one thread, so the two threads a
  machine of four cores gives sign in about 40 a second; the Linux tier,
  four threads beside its own build's tests, made 23.7.
- **The limits** (§2, amendment G): five failures from an address in a
  minute refuse the next with no hash and the seconds left; five on a
  skeleton in fifteen minutes make each next try on it wait 1, 2, 4 ... 32
  seconds before the pool, a right password or fifteen quiet minutes
  clearing it. The counts forget themselves once past their window.
- **Login names** (amendment F): ASCII, `A-Z a-z 0-9 _ . -`, 3 to 20; the
  skeleton folds case, `l I 1` and `o 0`. A sign-in finds the account whose
  name is the one typed in any case; a name that only looks like it is no
  account's.
- **The account at the door** (amendment C): with `[accounts]`, a hello's
  ticket is checked at the front door with the manager's key and spent there;
  a player coming back to a world, an instance or a place in line keeps the
  account the manager knew them by. Every pass names the account the
  manager knows a player by -- from the door, from a world's arrivals --
  inside what is signed, and a world takes `Player.AccountId` from it, a
  game's `OnJoinRequest` finding `joiner.AccountId` already set and a world
  with none admitting through the same ban and one session an account.
- **Tried in the editor** (ADR 0225): with `[accounts]` on, the manager the
  editor starts lets a player in who did not sign in as a test account,
  `trial-` and the start of their key, so a game's worlds are tried with no
  login screen.
- **The console's `account`** makes the passwords it sets -- sixteen letters
  and digits that do not look alike -- and shows each once; a password
  typed on its line is not taken, since every line is logged.
- **Proved** by the door's tests (an account made and signed in to, its
  ticket a world's to take; each refusal; an address's sixth failure
  refused at once; a name's waits doubling and cleared by its player; a full
  pool; the console), the manager's (a joiner who did not sign in refused at
  the door; a test account known by both worlds), the pass's (one whose
  account was changed after it was signed is not the manager's), and
  `data_accounts`: under a manager, a player's machine makes an account,
  meets `Taken`, `WrongNameOrPassword` for a wrong password and for a name
  with no account, and `InvalidPassword`, signs in with the name in another
  case and joins; the town and the fields know the player by the same
  account, the fields load the record the town changed in the tick of the
  hop, and the password is in no log of any process and not in the
  database's file.
- **Not yet**: the realms' login drawn and tried by a person; the editor's
  own test of a game with `[accounts]`.
