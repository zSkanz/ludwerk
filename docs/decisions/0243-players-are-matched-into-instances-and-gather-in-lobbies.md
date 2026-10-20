# 0243 — Players are matched into instances, and gather in lobbies

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0221 (instances), ADR 0222 (`TravelGroup`), ADR 0238 (the
  data service), ADR 0240 (parties)

## The question

An online game with arenas, dungeons or matches needs players brought
together by mode, region and skill, a party kept together, and a full match
the moment one is possible; and custom games where players gather by a code
and say they are ready. ADR 0221 already makes an instance for a group, and
ADR 0222 sends a group there. What is missing is choosing the group.

## Decision

### 1. `MatchmakingService`, kept by the manager

The manager holds the queues, so a player in any world can queue and be
matched with players in any other. On the server:

```luau
--!strict
local MatchmakingService = game:GetService("MatchmakingService")

MatchmakingService:DefineQueue("Arena2v2", {
    Template = "Arena",        -- the world an instance is made from (ADR 0221)
    Teams = 2, TeamSize = 2,
    Rating = "Arena",          -- a rating kept per account (§3); nil for none
    Widen = { After = 10, Step = 100, Most = 600 }, -- seconds, rating points
    Regions = true,            -- players are matched within their region first
})

local ticket = MatchmakingService:QueueAsync(player, "Arena2v2") -- or a party id: its online members, together
MatchmakingService:CancelAsync(ticket)
MatchmakingService.Matched:Connect(function(match: MatchInfo) end) -- in the world each matched player is in
```

**A match is formed when a queue holds enough tickets that fit**: the teams
filled from tickets whose ratings are within the window, a party never split
across teams or matches, the window widening `Step` every `After` seconds up
to `Most`. A region is the manager's machine's region in `servers.toml`
(`[manager] region`) unless the game passes one; with one manager a queue is
one region.

When a match forms, the manager makes an instance of the template (ADR 0221)
for exactly the matched players, raises `Matched` in their worlds, and sends
them there (`TravelGroup`, ADR 0222). **A player is in one queue and one
match at a time**: queueing again cancels the earlier ticket, and a player
already in a match is refused.

### 2. Backfill

A match whose player leaves can ask for one: `MatchmakingService:BackfillAsync
(match, team)` puts a ticket for the open place at the front of the queue,
filled by the first fitting player, who travels into the running instance.

### 3. Ratings

A rating is a number per account and queue's `Rating` name, kept in the data
service (ADR 0238), starting at 1,500. `MatchmakingService:ReportResultAsync
(match, { winners })` updates the ratings of the match's players with Elo (K
= 32); a game that wants another system writes the numbers itself with
`SetRatingAsync`.

### 4. Lobbies

For custom games, a lobby is a waiting room the manager keeps:

```luau
local lobby = MatchmakingService:CreateLobbyAsync(owner, { Template = "Arena", Size = 8, Public = true })
print(lobby.Code)                                  -- six letters, to give to friends
MatchmakingService:JoinLobbyAsync(player, "QXWBRT")
MatchmakingService:SetReadyAsync(player, true)
local lobbies = MatchmakingService:ListLobbiesAsync({ Template = "Arena" }) -- the public ones
MatchmakingService:StartLobbyAsync(lobby.Id)       -- its maker; refused until everyone is ready
```

A lobby's members wait in the world they are in; starting it makes the
instance and sends them, as a match does. An empty lobby is gone.

### 5. Elsewhere

`[matchmaking] provider = "nakama"` uses Nakama's matchmaker (ADR 0242) for
§1; the instance and the travel stay the manager's.

## Proof

- **200 bots in mixed parties** (solos, pairs, a four) queue at once for
  2v2 and 4v4 queues: every match is filled within its limits, no party is
  split, no player is in two matches, and the last match forms within the
  widening's `Most`.
- A player who leaves a match: backfill brings another in.
- Two wins raise a rating by Elo's numbers, and they are in the data service.
- A lobby by code: eight players join, start is refused until all are
  ready, then all eight are in one instance.

## Consequences

- New API: `MatchmakingService` (`DefineQueue`, `QueueAsync`,
  `CancelAsync`, `BackfillAsync`, `ReportResultAsync`, `GetRatingAsync`,
  `SetRatingAsync`, `CreateLobbyAsync`, `JoinLobbyAsync`, `LeaveLobbyAsync`,
  `SetReadyAsync`, `ListLobbiesAsync`, `StartLobbyAsync`, `Matched`,
  `LobbyChanged`), `MatchInfo`, `LobbyInfo`; `[matchmaking]` and `[manager]
  region` in `servers.toml`. No client protocol: a script asks its server.
- A match is an instance, so everything instances have -- a warm pool, a
  reconnect, a closed instance sending its players back -- a match has.

## As built (2026-10-19)

- **The book** is the manager's (`MatchBook`): queues as each world says
  them, tickets in the order they were made, and the matches and lobbies
  being played and waited in. Each frame it makes every match it can, for
  the oldest ticket first: the tickets within its window -- `Step` at first,
  `Step` more every `After` seconds, `Most` at most -- and, with `Regions`,
  in its region until the window is widest; then a bounded search that fills
  the teams, each ticket whole on one team.
- **A match is an instance**: the manager opens one of the template, lets in
  exactly its players, and once it is ready sends each world with any of them
  `Matched` and the hop, as a party's `TravelGroup` is sent. The instance
  reads `SceneService:GetLoadData().Match` (`Id`, `Queue`, `Teams`): the
  manager sends the match with the instance's scene, and the instance makes
  the load data of it itself, as a `RemoteEvent` carries it -- a template
  needs a `scene` for that. One it could not open (its limit, the machine's
  memory) lets the match go, its players free to queue again, and the
  operator is told.
- **A player who leaves** the instance, once in it, has left the match. A
  match ends when its instance closes.
- **Backfill** puts a place before every ticket of the queue, filled by the
  first lone ticket within the window of the match's mean rating.
- **Ratings** are the data service's shared values, scope `rating`, key
  `<rating>:<member>`, a number as a script writes one; written only by the
  manager's thread. `ReportResultAsync(match, winningTeam)` takes the winning
  team's number (1-based) rather than a table: a game with draws or places
  writes the numbers itself with `SetRatingAsync`.
- **Lobbies**: `StartLobbyAsync(lobby, by)` takes the player starting it, who
  must be its owner; a lobby started is a match of one team, with no queue
  and so no rating. `LobbyChanged` fires once more with no `Members` when a
  lobby is gone.
- **Ids and codes** are random letters (`m-`, `t-`, `l-` and a count before
  them); a code is six capitals, joined in any case.
- **Proved**: in the book's own tests, 200 players in mixed parties -- 40
  solos and 30 pairs for 2v2, 40 solos, 20 pairs and 5 fours for 4v4,
  ratings from 1,200 to 1,800 -- queued at once: 25 and 12 matches, every
  one full, no party split, nobody in two, the last at 50 seconds, when the
  window is widest; the window's widening, regions, backfill, lobbies and
  Elo's numbers. Under the manager, with worlds and players: a party and
  three players queued in the town, two teams of two sent to one instance
  that read its match from its load data, two wins taking a rating from
  1,500 to 1,530.53 (and the loser's to 1,469.47) in the data service, a
  player who left replaced by the one who waited; and eight players in a
  lobby by its code, refused to start until all were ready and by anyone
  but its owner, then all eight in one instance. `[matchmaking] provider` is
  C6's (ADR 0242).

## Not decided here

Matching across several managers (regions on several machines need a shared
queue, which is server meshing's neighbour and out of scope), and anti-smurf
measures beyond the rating.
