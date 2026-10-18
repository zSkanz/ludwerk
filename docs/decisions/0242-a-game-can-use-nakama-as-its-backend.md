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
