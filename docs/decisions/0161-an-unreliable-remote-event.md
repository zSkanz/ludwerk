# 0161 — An unreliable remote event: sent once, the newest wins, at most 16 KiB

- Status: accepted
- Date: 2026-10-04
- Decided by: the agent, on the owner's approval of the batch through ludwerk-08
  (2026-10-04), under the standing rule to decide as professional engines do
  and record it. It is item F (N10) of `docs/briefs/netcode-acceptance.md`.
- Amends: ADR 0077, whose `RemoteEvent` is the only way a script sends, and is
  reliable

## Context

A script has one way to send: `RemoteEvent`, on the reliable, ordered Control
channel. That is right for "I bought the sword". It is wrong for anything a
game sends many times a second and replaces each time: a horde's positions,
where a player is aiming, a boss's health bar.

- A reliable message that is lost is sent again, and **every reliable message
  behind it waits** -- spawns, despawns, the roster and every other event of
  the game ride the same channel. One lost packet of a snapshot that the next
  one would have replaced a fifteenth of a second later stalls all of it.
- The test game that found this (a horde of a thousand, mirrored to friends
  by script at 15 Hz, 5 to 14 KB a message) had nothing else to send it on.

## How mature engines do it

- **Unreal**: a remote procedure call is declared `Reliable` or `Unreliable`,
  and the documentation's advice is that anything sent every frame is
  unreliable.
- **Unity Netcode for GameObjects**: an RPC carries a delivery --
  `Reliable`, `Unreliable`, `UnreliableSequenced`, and fragmented reliable for
  large ones. Unreliable RPCs above the transport's MTU are not sent.
- **Godot**: `@rpc("unreliable")` and `@rpc("unreliable_ordered")` beside
  `"reliable"`, per method, with a channel number.

They agree that reliability belongs to the declaration, not to each call, and
that the unreliable kind is never retransmitted and never held for.

## Decision

1. **A class beside `RemoteEvent`: `UnreliableRemoteEvent`**, with the same
   surface -- `FireServer`, `FireClient`, `FireAllClients`, `ServerReceived`,
   `ClientReceived` -- and the same values. Which kind an event is is read
   from its type, wherever it is fired. It is created on the authority and
   replicates as a `RemoteEvent` does.
2. **Sent once, and never held.** A message goes out in the frame it was
   fired, or not at all:
   - to a peer that does not know the event yet, it is dropped, not kept;
   - from an event with no network id yet (made this tick), it is dropped;
   - from a replica not welcomed yet, it is dropped;
   - with nobody listening on the other side, it is dropped, not kept for a
     first listener.
3. **The newest wins.** Every unreliable message carries a number that
   counts up for its connection, and the receiver drops one that is not newer
   than the last it took: a message that arrives after a later one is dropped
   rather than delivered late. Its own channel -- `Remote`, number 4 -- is
   unreliable and sequenced too, so the transport drops most of them first.
   The number is the connection's, so this holds across every unreliable
   event of a game, not for each alone; a game must not read an order into
   them, only that what it hears is never older than what it heard before.
4. **At most 16 KiB of arguments**, refused at the call with its own error
   (`net.err.unreliable_too_large`). That is what a snapshot is allowed
   before it goes reliable in parts, for the same reason: past one packet
   (about 1.2 KB) the transport sends a message in fragments, and **a message
   is lost whole when any one fragment is**. At 2% loss a 1 KB message arrives
   98 times in 100, a 5 KB one about 90, a 15 KB one about 75. A game sends
   small, or sends in pieces that each stand alone.
5. **A client is not trusted with it either.** The authority takes a client's
   unreliable messages from the same budgets as its reliable ones (messages
   and bytes a tick), learns the sender from the connection, and drops one
   that names anything but an `UnreliableRemoteEvent`. A reliable message
   naming an unreliable event is dropped too: the kind is the event's, not
   the sender's choice.
6. **On an authority it is delivered as the reliable one is**: `FireServer`
   reaches its own `ServerReceived`, and a host's message to its own player
   its own `ClientReceived`. Nothing is lost with no wire to cross. One script
   runs solo, hosting and networked.
7. **The wire is protocol 38**: channel 4 (`Remote`, unreliable sequenced),
   and messages 25 (`UnreliableToAuthority`) and 26 (`UnreliableToReplica`) --
   the message's number, the event's network id, the instances the arguments
   name, and the arguments to the end of the message.
8. **Counted**: `NetworkService:GetStats()` gains `UnreliableSent`,
   `UnreliableReceived` and `UnreliableDropped` -- what this machine sent,
   took in, and refused or dropped itself. What the network lost in between
   is `Loss`, as before; no end can count a message that never arrived.

## Why a class and not a property

A `Reliable` property on `RemoteEvent` would be one class fewer. It would also
be a switch a script could flip between two fires, replicated a tick later
than the fire that depended on it, and a 64 KiB event that became a 16 KiB one
while a message was in flight. The declaration is the event; a different
contract is a different type, and a reader of `events.Horde:FireAllClients`
sees which it is where the event is made.

## Consequences

- A game's state that is replaced many times a second goes on an
  `UnreliableRemoteEvent`, and no longer delays anything else.
- A game cannot use it for anything that must arrive. The manual says so
  first.
- Tests: over a transport that loses a third of what is sent, the unreliable
  messages that arrive are whole, never out of order, and fewer than were
  sent, while the reliable ones all arrive; a message to an event the peer
  has not got is dropped and not held; a client's flood is cut by the same
  budget; a message over the limit is refused at the call; and a 15 KB
  message crosses the real transport in fragments.
