# 0242 — A game can use Nakama as its backend, through an optional integration

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0188 (optional integrations), ADR 0226 (tickets), ADR 0237
  (accounts), ADR 0238 (the data service and its backend contract), ADR 0243
  (matchmaking)

## The question

Some studios already run Nakama -- an open-source game backend (Apache-2.0)
with accounts, storage, a matchmaker, parties and leaderboards -- or want a
backend that is not the manager on one machine. They should be able to put a
game made with the engine behind it without writing the glue, and a game that
does not use it should carry none of it.

## Decision

### 1. An integration, off unless a project enables it

`nakama` is an integration under ADR 0188: declared in the API schema, enabled
per project (`[integrations] nakama = true`), and absent from a game that
does not enable it. It needs **no SDK and no native library**: Nakama speaks
HTTP and JSON, and the integration is Luau on the engine's own HTTP client
(`net.request`, ADR 0223) plus a server module that runs inside Nakama. Nothing
of it is in `third_party/` or in the engine's core.

### 2. What it connects

- **Accounts and tickets.** A client signs in to Nakama (device, e-mail or a
  platform's token, as Nakama allows) with `NakamaService:AuthenticateAsync`.
  Nakama's own session token is not what a world verifies: a small runtime
  module the integration ships for Nakama (TypeScript, Apache-2.0, ours)
  exposes an RPC that turns a valid session into the engine's ticket -- a JWT
  signed with an Ed25519 key kept in Nakama's configuration, which the worlds
  are given as `ticket_public_key` (ADR 0226). From there a join is as any.
- **Data.** The data service's backend contract (ADR 0238 §5) is implemented
  by the same runtime module on Nakama's storage engine: a player's record, a
  transaction (in one Nakama storage write with version checks), shared values
  with versions. `[data] backend = "nakama"` points the worlds at it.
- **Matchmaking.** `MatchmakingService` (ADR 0243) with `[matchmaking]
  provider = "nakama"` puts tickets in Nakama's matchmaker instead of the
  manager's; a match found there is handed to the manager, which makes the
  instance (ADR 0221) and sends its players there as ADR 0243 does.
- **Leaderboards** stay `LeaderboardService`'s (ADR 0188's platform
  integrations); a Nakama provider for it is a later step.

### 3. What it does not do

It does not run worlds: the worlds are still the engine's, under its manager.
It does not replace `PartyService` (ADR 0240): a party is the manager's,
because travel and instances are.

## Proof

- Against a Nakama server in a container (its published image, pinned):
  sign in with a device id, receive a ticket, join a world that verifies it.
- A record written by one world and read by another through Nakama; the
  thousand-trade test of ADR 0238 against Nakama's storage, nothing lost or
  doubled.
- Two clients queued through Nakama's matchmaker land in one instance.
- A project without the integration enabled: no `NakamaService`
  suggestions, and calling it fails with an explicit reason (ADR 0188).

## Consequences

- New API, integration-owned: `NakamaService` (`AuthenticateAsync`,
  `GetTicketAsync`, `Session`); `[integrations] nakama`; `[data] backend =
  "nakama"`; `[matchmaking] provider = "nakama"`; the runtime module under
  `integrations/nakama/`.
- The engine's own path -- manager, data service, matchmaking -- stays the
  default and needs nothing else.

## Not decided here

Nakama's realtime socket (chat, presence): `@engine/chat` (ADR 0239) carries
chat over the engine's own worlds, and a game that wants Nakama's chat uses
it through the same HTTP client.

## Amended 2026-10-20: the runtime module in Lua, and who a session is asked of Nakama

Two corrections before any of it was built, approved with the design of the
phase's last lot.

- **The runtime module is Lua**, not TypeScript. Nakama loads `.lua`
  modules from its `modules` folder as they are; TypeScript needs `tsc` and
  Node to build one file, a toolchain the repository has nowhere else. Lua is
  also the language nearest the engine's own. It is still ours, Apache-2.0,
  under `integrations/nakama/`.
- **The front door asks Nakama who a session is**, rather than Nakama
  signing the engine's ticket. Nakama's runtime signs a JWT with HS256 or
  RS256 only (`jwt_generate`, its Lua function reference), and the engine
  verifies Ed25519 tickets only (ADR 0226). So the player's machine joins
  with its Nakama session token in the ticket's place, and the front door
  calls the module's `engine_whoami` RPC with that token as the session's
  bearer: Nakama checks it and the RPC answers the user id, which is the
  player's account. No key is shared between them, and nothing new is
  verified on the engine's side.
- **Matchmaking through Nakama waits for research**: the server runtime's
  Lua functions add no matchmaker ticket for a user (only
  `register_matchmaker_matched` is offered). Whether a provider can work
  without a realtime socket held for each player is to be found out first,
  and its cost written up if it cannot.

## As built, signing in and the data (2026-10-20)

- **The module** is `integrations/nakama/modules/engine.lua`, Lua, Apache-2.0,
  ours. `engine_whoami` answers the user of the session that called it, and
  refuses a call with no session. `engine_data` takes the data service's
  contract (ADR 0238 §5), from the server's runtime HTTP key alone, on
  objects the system owns: records, items, the list of items each holder has,
  balances, shared values, two counters, and each request's answer. A change
  reads what it touches and writes it all in one `storage_write`, each object
  at the version it was read and the request's record at `*` -- Nakama writes
  a batch whole or not at all -- so a request sent again is answered as it
  was, and a write refused for a version is read again and tried, eight
  times at most.
- **Signing in** (protocol 62): `SignIn`'s first byte is its kind -- 0 sign
  in, 1 make the account, 2 a session of the game's Nakama, its token in the
  password's place, up to 4096 bytes. The front door's pool asks
  `engine_whoami` with the session as its bearer, under the address's limit;
  a session Nakama does not know, or none, is `WrongNameOrPassword`, and a
  door with no `[nakama] url` refuses one. The ticket is the engine's own,
  for `nakama:<user id>`.
- **`NakamaService`** (integration `nakama`, ADR 0188): `AuthenticateAsync`
  (`device`, `email`, `custom`) on the engine's HTTP client, the client key
  from `[integrations.nakama] configuration`, `http://<key>@host:port`;
  `Session`, a fact of this machine kept out of the world's hash; and
  `GetTicketAsync`, the sign-in above. A project without the integration
  gets `IntegrationDisabled` and, from `GetTicketAsync`, a door answering no
  session.
- **`servers.toml`**: `[nakama] url`, under the same rule as a backend's for
  `http://`, and `http_key_env`; `[data] backend = "nakama"` is refused
  without a `[nakama] url`.
- **The proof's Nakama** is `scripts/localgate.ps1 -Only nakama`, opt-in:
  `heroiclabs/nakama:3.41.0` and `postgres:16.10-alpine`, each pinned by
  digest, pulled and run on the machine for the test and shipped nowhere --
  a test's dependency, Nakama Apache-2.0 and PostgreSQL permissive -- on a
  network of the run's own, removed whatever happens.
