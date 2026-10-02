# Netcode acceptance: the kickoff and the ledger

Opened on 2026-10-02 by the owner's word, through ludwerk-08: *the multiplayer
follows the architecture professional engines follow, and it is fluid whatever
the ping* -- he plays a tile MMO on his phone and feels no delay, and he is
testing three games with friends over a VPN this week. This ledger is how that
is proved and what stands in the way of it.

**What we already have is the standard model**: client-side prediction and
reconciliation by replay (ADR 0076), interpolation between snapshots, predicted
physics (ADR 0133), an input buffer with redundancy, interest. What is missing
is the ability to prove it under a bad connection, and the defects an audit of
it found.

**Place in the queue:** ahead of the rest of batch 3c and of
`network-kickoff.md`'s C items, which fold in where they belong. 3c resumes
after (`settings-kickoff.md` G4's rest, mobile M2-M3, the pt-BR catalog, the
manipulator pictures).

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A failing test or a failing measurement first -- and for anything the memory
  transport does not model (its size cap, fragments, the throttle), over the
  ENet loopback: the memory transport enforces none of them, which is why the
  suite was green on all of what follows.
- Defaults where a script says nothing are the industry's: snapshots at 20 to
  30 a second, an interpolation delay of two snapshot intervals that adapts to
  the jitter, input redundancy of three or four ticks, reconciliation by replay
  smoothing a VISUAL offset and never moving the simulated body smoothly. Where
  the code already does this, the gate only has to show it.
- Never port 7777 (the owner's live server) nor 7778 (a game's real server).
- One push per lettered item; "package whole" after A+B, after C-E, after F-H.

## A -- a network-condition simulator for real processes (first)

- [ ] `engine-host --net-delay=MS --net-jitter=MS --net-loss=PCT`, each way,
  in dev and test hosts and refused by a shipping build. Below ENet, so ENet
  itself sees the conditions -- its round trip, its retransmits, its throttle --
  as a UDP link conditioner between the transport's socket and the network.
- [ ] The same reachable from a spec and from the C++ tests over the loopback.

## B -- the acceptance gate

At 0, 50, 150 and 300 ms round trip, 2% loss, +-20 ms jitter, and with a
150 ms and a 400 ms frame on the client:

- [ ] Own character -- a `CharacterBody` in 3D, a `Character2D`, and one a
  script predicts: input shows on the frame it is pressed; a straight walk of
  10 s is zero corrections; stopping, the authority stops within 1e-3 of the
  replica.
- [ ] Other players: no jump in a drawn position larger than one tick's travel
  between two frames; the interpolation delay adapts to the jitter and
  `GetStats()` says what it is.
- [ ] A reliable remote arrives once and in order at every condition; a
  remote's round trip is at most the ping, a tick and a frame.
- [ ] `GetStats().Ping` and a remote's round trip do not change when the
  window loses the front or is minimised.
- [ ] The numbers per condition in one table, kept as a golden the way the
  frame budgets are.

## C -- the transport serviced on its own clock (N9)

- [ ] The headless server waits inside the transport for the time left to the
  tick (acks leave at once; the simulation still consumes at the tick); a
  client services it every frame and flushes after a tick's sends; then on a
  clock of its own, so a window at ten frames a second still answers in
  milliseconds. `Ping` is the transport's round trip. Say what slows a window
  in the background (our `backgroundFrameRate`) and that its ticks keep sixty
  a second.

## D -- the input stream

- [x] D480: a client heard again after a long frame -- anchored again when its
  ticks fall behind (at once past the redundancy window, else after four late
  ticks in a row), on the newest tick that came; late and skipped presses
  carried; a stand-in that lives half a second of silence.
- [ ] N13: `NetworkService.State` during a redial, and a `Reconnecting` state
  or signal so a game can show it and stop predicting.

## E -- the lifecycle

- [ ] N1: a `Join` nobody answers leaves the machine as it was and fires
  `JoinFailed`; N2: after `Disconnected` a client goes back to the scene it
  joined from, never runs the server's scene alone, and `UserId` is 1 again.
- [ ] NA7-NA13 (below).

## F -- unreliable remote events (N10, an amendment to ADR 0077)

- [ ] A class beside `RemoteEvent` with its surface, its own channel without
  retransmission, never fragmented on the reliable path, counted in
  `GetStats()`; a spec over a lossy transport.

## G -- lag compensation for hits (an amendment to ADRs 0076 and 0133)

- [ ] The authority keeps about a second of history of the bodies a script
  marks; a raycast or overlap "as player P saw it" goes back by P's
  interpolation delay and half its round trip, capped at 200 ms.

## H -- the rest of the network findings

- [ ] N11 (where a long frame's time went), N12 (the world replaced on join
  costs a frame of 80-180 ms), N3-N6, N8 (`lote 6`'s list in
  `network-kickoff.md`).

## The audit's findings (NA1-NA33)

ludwerk-08's audit of 2026-10-02 against what professional engines do, with
the file and line of each and a failing test for each. **Measured** ones were
reproduced with real processes. Order: after A and B, the P0s; the P1s where
they belong in C-H; the P2s as they fit.

### P0 -- breaks play

- [ ] **NA1** Joining a world of more than a few hundred parts takes tens of
  seconds or never ends (measured: 1 000 parts 29 s, 5 000 never, on
  localhost): the first snapshot whole, unreliable and fragmented, resent
  every two ticks; a silent 1 MiB send cap; acks not checked; the atom count a
  u16 (NA33). Initial state reliable and in chunks, or under a send budget.
- [ ] **NA2** Block and terrain ground ignores interest and goes whole to every
  joiner on the reliable control channel, encoded again per peer.
- [x] **NA3** The input stream after long frames (D480's follow-up).
  Left for C: `MaxIntentsPerTick` drops the newest messages after a server
  hitch, and a headless server drops simulated time past four catch-up ticks.
- [ ] **NA4** A window that is not in front runs ten frames a second in a
  networked session too, the listen host included -- every client sees the
  others freeze and jump, and the ping reads 100 ms.
- [ ] **NA5** ENet at its defaults: the throttle drops snapshots and intents
  after a spike for up to five seconds; an MTU too large for a VPN; Windows'
  connection-reset reports cut a frame's receive short.

### P1 -- visible, fragile or exploitable

- [ ] **NA6** N7's cause: the reconcile that takes the authority's place keeps
  samples newer than the acknowledged tick, and the next answer adds the error
  twice (measured: the own body at twice its position after a server teleport).
- [ ] **NA7** `Host` failing is silent (measured). **NA8** A full server
  answers nothing (measured); no max-players key, no kick, no approval hook.
  **NA9** `Disconnect` throws away the same tick's sends. **NA10** The network
  is not serviced during a server's `BindToClose`. **NA11** Name resolution
  blocks the frame and is IPv4 only. **NA12** Seconds frozen after a short VPN
  drop. **NA13** Android in the background ends the match.
- [ ] **NA14** The interpolation clock moves only forward, its delay is fixed,
  and nothing extrapolates. **NA15** Teleports are interpolated. **NA16**
  Correction smoothing pops on its first frame; no rotation smoothing; 2D has
  neither replay nor smoothing. **NA17** A part a client creates cannot move on
  that client. **NA18** The own character collides with others where they were
  drawn. **NA25** Input read once a frame, after the ticks: taps lost at 30 fps.
- [ ] **NA19** Send cost grows as players times instances, with 64 world copies
  each side. **NA20** No bandwidth budget, priority or quantisation. **NA21**
  One remote the peer does not hold blocks all remotes for five seconds.
  **NA22** Reliable remotes dropped after a hitch. **NA23** Flood limits far
  above any game, with no consequence. **NA24** A drag detector's ownership
  lets a client teleport a body.

### P2 -- gaps against professional practice

- [ ] **NA26** No clock synchronisation; the delay rises fast and falls only
  while idle. **NA27** `GetStats()` counters that do not mean what they say.
  **NA28** Timeouts promised and not wired. **NA29** Half-open connections and
  connect floods. **NA30** The token in clear (goes with ADR 0120's build), any
  captured network id nameable, a player with no character sent the whole
  world. **NA31** Attributes re-encoded every snapshot; long strings cut
  silently. **NA32** The replica's despawn, release and ack details. **NA33**
  The atom count's overflow (with NA1).

## Findings
