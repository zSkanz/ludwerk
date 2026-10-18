# 0240 — A party is the manager's, and a guild is data

- Status: accepted
- Date: 2026-10-17
- Builds on: ADR 0220 (the manager), ADR 0221 (instances), ADR 0222
  (travel), ADR 0230 (channels), ADR 0238 (the data service)

## The question

Players of an online game play together: a party that walks into a dungeon
instance as one, follows its leader to another world, queues for an arena
together; a guild that outlives every session, with ranks and what each rank
may do. A party is short-lived and must be known wherever its members are; a
guild is long-lived and its rules are the game's.

## Decision

### 1. `PartyService`, kept by the manager

A party lives in the manager, which sees every world, channel and instance,
so it crosses all of them. On the server:

```luau
--!strict
local PartyService = game:GetService("PartyService")

local party = PartyService:CreateAsync(leader)          -- a PartyInfo
PartyService:InviteAsync(party.Id, leader, invitee)       -- the invitee's world raises Invited
PartyService:AcceptAsync(party.Id, invitee)
PartyService:LeaveAsync(member)
PartyService:SetLeaderAsync(party.Id, member)
local mine = PartyService:GetPartyAsync(player)            -- nil, or { Id, Leader, Members }
PartyService.Changed:Connect(function(partyId: string) end) -- raised in every world a member is in
```

Members are accounts (ADR 0237), so a member who drops and comes back is
still in it; a party with nobody online for five minutes is gone. At most
eight members by default (`[parties] size`), and a player is in one party.

**What a party is for, in the engine**: `TravelGroup` (ADR 0222) takes a
party's id and moves its members who are online; `CreateInstanceAsync` (ADR
0221) takes it as the group the instance is for; matchmaking (ADR 0243)
queues it as one. Everything else -- loot rules, shared quests -- is the
game's, on `Changed` and `GetPartyAsync`.

A server with no manager keeps parties in its own memory, for its own world;
the API is the same.

### 2. Guilds: `@engine/guild`, on the data service

A guild is data: a name, members with ranks, and what each rank may do. It is
kept in the data service's shared values (ADR 0238 §4, scope `guild`), with
versions, so two worlds changing it at once never lose a change. It is a
Luau module, required by a game that wants it, because its rules are the
game's:

```luau
--!strict
local guild = require("@engine/guild")

local g = guild.create(founder, "Ironhold")
guild.addRank(g, "Officer", { "invite", "kick", "bank.withdraw" })
guild.invite(g, by, invitee)        -- refused unless `by`'s rank has "invite"
guild.setRank(g, by, member, "Officer")
guild.can(g, member, "bank.withdraw")
guild.changed:Connect(function(id: string) end)
```

Permissions are strings the game names; the module checks them and does
nothing else with them. A guild bank's money and items are the data
service's (ADR 0238 §3), owned by the guild's id. Guild chat is
`@engine/chat`'s guild channel (ADR 0239).

## Proof

- A party of four in three worlds: `Changed` reaches all three; the leader's
  `TravelGroup` brings the two online members to the leader's world; an
  instance made for the party admits its members and nobody else.
- A member who disconnects and returns within five minutes is still in it.
- Two worlds promoting two members of one guild at once: both promotions are
  kept.
- `guild.can` refuses a permission a rank does not have.

## Consequences

- New API: `PartyService` (`CreateAsync`, `InviteAsync`, `AcceptAsync`,
  `DeclineAsync`, `LeaveAsync`, `KickAsync`, `SetLeaderAsync`,
  `GetPartyAsync`, `Changed`, `Invited`), `PartyInfo`; `TravelGroup` and
  `CreateInstanceAsync` take a party id; `[parties]` in `servers.toml`;
  `@engine/guild`. Protocol: none to clients; manager pipe messages.
- A guild has no class of its own and no engine code; a game with another
  idea of a guild writes it on the same data service.

## Not decided here

Alliances of guilds, guild housing and guild ranking: a game's, on these.
