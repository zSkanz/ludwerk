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

## As built (2026-10-19)

- **The manager keeps the parties** (`PartyBook`): each a leader, members and
  those invited, by member id -- the account where the player signed in, the
  key where they did not (`PartyService:MemberIdOf`). A world asks the manager
  over its pipe (`party-op`) and is answered (`party-result`); the manager
  says `party-changed` to every world a member is in, and `party-invited` to
  the invitee's. A server with no manager keeps a book of its own.
- **`InviteAsync`** takes a `Player` or a member id, so a party invites a
  friend in another world. `KickAsync` and `SetLeaderAsync` take a member id.
- **`TravelGroup`** takes a party's id: each world sends the members it has,
  and returns the ones in the asking world. **`CreateInstanceAsync`** takes
  `Party`, whose members online, in any world, are the instance's group.
- **Guilds** are `runtime/engine/guild/init.luau`, in the data service's
  scope `guild`: the guild under its id, its name claimed under
  `name:<lower case>`, and each member's guild under `member:<id>`, a claim
  let go of written as `false`. Each change is an `UpdateAsync` of the guild,
  and `guild.changed` is a `CrossWorldService` topic.
- **Proved** in the manager's tests: a party of three in two worlds, invited
  where its member is, `Changed` heard in both, sent together with
  `TravelGroup`; an instance made for a party of two taking both and refusing
  the third; a guild made once, its name taken in any case, a member invited
  by id joining in the other world, two changes to it at once both kept, and
  `guild.can` refusing what a rank does not have. A member offline and back
  within five minutes is still in the party, and one gone longer is not, in
  `PartyBook`'s own tests. The test of two changes at once makes them from
  two threads of one world; two worlds go through the same data service, as
  ADR 0238's tests prove.

## Not decided here

Alliances of guilds, guild housing and guild ranking: a game's, on these.
