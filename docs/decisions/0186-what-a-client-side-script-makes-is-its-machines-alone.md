# 0186 — What a client-side script makes is its machine's alone, a host's included

- Status: accepted
- Date: 2026-10-06
- Decided by: the owner, in his words relayed by the coordinator -- "things
  created by the server replicate, things created by the client do not, even
  when the host is the host; only scripts that run on the server side should
  replicate the instances they create" -- and shaped by the agent
- Builds on: ADR 0105 and ADR 0138 (which side a script runs on), ADR 0137
  (a script's run), ADR 0184 (`BasePart.Fade`, the same question asked of a
  property)

## Context

A machine that joined a match sends nothing of its world: what its scripts
make is its own, as the manual says. A machine that HOSTS is a server and a
client in one world, and everything in that world that the wire describes was
sent -- whoever made it. A game's client code put a ring under the hero and a
number over an enemy, under `Workspace`, and on a host those were sent to
every friend, who drew the host's over their own and was sent their motion
every frame.

The way round it was to parent what is local under a `Camera`, which does not
replicate. It works, and it is a trick a game's author has to have been told.

## How mature engines do it

In both of the engines this one follows, what is made on a client is that
client's: replication is of what the server made, and a listen server's local
player is no exception. The server is where a thing has to be made to be
everyone's.

## Decision

1. **An instance made by a script that does not run on the server side is
   local**: this machine's alone, on every machine, a host included. It is
   never sent, with everything under it, wherever it is parented -- under
   `Workspace`, or under an instance that does travel. `Instance.new`,
   `Instance.stamp` and `Clone` all say so of what they return; a clone and a
   placed stamp are marked whole, so a piece moved out from under its root is
   still nobody else's. `Clone` follows who calls it, not what is copied.

2. **The server side is** `ServerScriptService`, `GlobalScriptService.Server`,
   and a script elsewhere with `RunContext = Server`. Everything else is not:
   the client services, `RunContext = Client`, and a script every machine
   runs (`RunContext = Shared` outside the services). That last one is the
   owner's sentence read strictly, and it is also the only reading that makes
   sense: every machine ran the script and made its own, and a host's copy
   sent to a friend was a second one.

3. **What nobody's script made travels as it did**: a scene's instances, what
   the engine makes of its own, what the editor makes.

4. **A thread carries the side of the run that started it** (`ThreadSide`,
   kept in the Luau thread's own data). A script's thread is given its
   script's side. A thread made from another starts as that one -- a spawned,
   deferred or delayed task, a coroutine, and a module's body as it is
   required. A signal's handler runs as the side of the thread that
   CONNECTED it, whatever fired it. Any other callback -- a render step, an
   intent, a question's answer -- takes the side of the run its function was
   written in. So a function in a module both sides require makes what the
   side that called it would have made, which is the case a rule keyed on
   where the function was written gets wrong.

5. **`Instance.Local`**, read-only: whether the instance was made local. The
   named way to ask, where the `Camera` was the unnamed one. Not saved, not in
   the world hash, and never on the wire.

6. **Writes are not creations, and cannot follow.** A host is one world with
   one value of each property: a client-side script on a host that writes a
   property of an instance that travels has written it for everybody. That is
   stated in the manual beside the rule, with what to use instead -- make the
   thing on the client side, or say it with a property that is not sent
   (`BasePart.Fade`). And it can be found: **`--net-log-client-writes`** logs,
   once a script and property, each such write on a machine that is hosting
   -- properties, `SetMaterialParameter`, `SetAttribute` and `AddTag`. A key
   and not a standing warning, because a host's client code legitimately
   writes things that must travel, and a warning that cries on those is one
   nobody reads.

7. **The wire is unchanged** (protocol 41): a host sends less.

## What it does not do

- Something a server-side script made, parented under a local instance, is
  not sent either: a replica has no parent to give it. The same as under a
  `Camera`.
- `Local` says who made the instance. It is not the answer to "is this sent":
  an instance under a `Camera`, or of a class the wire does not describe, is
  not sent and is not `Local`.
- There is no way to make something local from the server side, or to make a
  client-side script's instance travel. A friend sees what a server-side
  script made; a client asks the server for it with a `RemoteEvent`.
- A module's own state is still one table on a host, shared by both sides, as
  it was.

## Consequences

- **Breaking for a game whose host made, from a client-side script, something
  its friends were meant to see.** It moves to the server side. In this
  repository that was three network tests' fixtures; the examples that build
  their world from `src/client/` are single-player samples, and the two that
  are matches (15, 30) already build theirs from `src/server/`.
- A game's client visuals are local without the `Camera`, and a host's
  capture walks that much less every tick.
- Tests: two machines, one hosting -- what its client side makes (straight,
  in a shared module, spawned, deferred, delayed, in a coroutine, from a
  signal connected through a shared module, cloned, parented under what
  travels) is on the host and not on the friend, and what its server side
  makes is on both; a script every machine runs makes one on each; and the
  write log says a property and a method once each.
