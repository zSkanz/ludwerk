# Netcode acceptance: the kickoff and the ledger

Opened on 2026-10-02 by the owner's word, through ludwerk-08: *the multiplayer
follows the architecture professional engines follow, and it is fluid whatever
the ping* -- he plays a tile MMO on his phone and feels no delay, and he is
testing three games with friends over a VPN this week. This ledger is how that
is proved and what stands in the way of it.

**What we already have is the standard model**: client-side prediction and
reconciliation by replay (ADR 0076), interpolation between snapshots, predicted
physics (ADR 0133), an input buffer with redundancy, interest. What was missing
was the ability to prove it under a bad connection, and the defects an audit of
it found.

**Place in the queue:** ahead of the rest of batch 3c and of
`network-kickoff.md`'s C items, which fold in where they belong. 3c resumes
after (`settings-kickoff.md` G4's rest, mobile M2-M3, the pt-BR catalog, the
manipulator pictures).

**The owner's ruling of the same night:** the audit fixed in one batch, grouped
by root, the full gate once at the end, one package and one report -- not a
cycle a finding. What blocks (a decision of the owner's, an amendment) is
listed and passed over.

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## What must hold at every stage

- A failing test or a failing measurement, and for anything the memory
  transport does not model (its size cap, fragments, the throttle), over the
  ENet loopback: the memory transport enforces none of them, which is why the
  suite was green on all of what follows.
- Defaults where a script says nothing are the industry's: snapshots at 20 to
  30 a second, an interpolation delay of two snapshot intervals that adapts to
  the jitter, input redundancy of three or four ticks, reconciliation by replay
  smoothing a VISUAL offset and never moving the simulated body smoothly.
- Never port 7777 (the owner's live server) nor 7778 (a game's real server).

## A -- a network-condition simulator for real processes

- [x] `engine-host --net-delay=MS --net-jitter=MS --net-loss=PCT`: a link
  conditioner, a relay in the process below ENet, holding, jittering and losing
  each datagram both ways on the link a client joins over; ENet's own round
  trip, resends and timeouts see it (a 50 ms delay reads a 100 ms ping). Dev
  and test hosts; a shipping build refuses it.
- [x] Reachable from the C++ tests through `TransportConfig::simulated*`.
- [ ] Conditions on a server's side, for every peer it holds.

## B -- the acceptance gate

- [x] `netcode_acceptance` (`tests/netcode/run_netcode_acceptance.cmake`): a
  real server and a real client, a `CharacterBody` predicted with the code the
  server drives it with, walked straight for ten seconds by a bot, at 0, 50,
  150 and 300 ms round trip with 2% loss and 20 ms of jitter, and with frames
  of 150 and 400 ms. Held to: no correction on the walk (two at most for a long
  frame), stopped within a millimetre of the authority, a remote's median
  round trip within the ping, a tick and a frame. The table is written beside
  it; see Findings for the one measured.
- [x] `GetStats()`: `Corrections` (counted), `InterpolationDelay`,
  `InputReanchors` beside what was there.
- [x] Long frames counted on both machines (over three ticks), and the gate
  run alone: CI's Windows runner, two cores shared with the suite, cost one or
  two corrections a walk through server stalls the client's own count never
  saw. The client draws nothing (`--rhi=null`), so a runner with no GPU runs it.
- [ ] A `Character2D` and a script-moved character in the same gate, and the
  others' drawn positions held to a tick's travel between frames.

## C -- the transport serviced on its own clock

- [x] N9: ENet is serviced by a thread of its own; a tick's sends are flushed
  together. A window in the background reads the ping a window in front does.
- [x] NA4: a networked session in the background runs at the simulation's
  sixty frames, the listen host included.
- [x] NA5: the packet throttle pinned, a 1200-byte datagram, Windows'
  connection-reset reports off; a disconnect after the queue.
- [x] A headless server catches up thirty ticks rather than four; intents are
  budgeted over time rather than eight a tick (the newest were dropped after a
  server hitch).
- [ ] NA12: a capped retransmit timeout -- inside ENet's protocol, so a patch
  under `third_party/patches`.

## D -- the input stream

- [x] D480: a client heard again after a long frame, to the last part (see the
  defect).
- [x] N13: `Enum.NetworkState.Reconnecting` while a dropped connection is
  dialled again.
- [x] NA25: a key pressed and let go between two ticks is down for one of them.
  The frame's events are still read after its ticks, a frame late.

## E -- the lifecycle

- [x] N1: a `Join` changes nothing until the server takes it; one that fails
  leaves the game as it was and fires `JoinFailed` with the reason.
- [x] N2: leaving returns to the scene the join was made from, solo, player 1.
- [x] NA7 `HostFailed`; NA8 a full server and one of another protocol answer
  `Refused` with the reason; NA9 sends before `Disconnect` arrive; NA10 the
  network is served while `BindToClose` runs; NA11 an address is read, not
  looked up, and `host:77x` is refused rather than dialled at 7777; NA28
  `[network] timeout` is the join's too; NA29 a connection that never says who
  it is goes in three seconds and frees its slot at once.
- [ ] NA11's IPv6 (ENet is IPv4-only); NA13 a phone in the background past ten
  seconds (the transport's thread keeps the link, the session's silence rule
  ends it: a "paused" notice with a longer grace).
- [x] N5: `--saves=DIR`, and a dedicated server keeps `saves-server` beside the
  player's `saves`.

## F -- unreliable remote events (N10, an amendment to ADR 0077)

- [ ] A class beside `RemoteEvent` with its surface, its own channel without
  retransmission, never fragmented on the reliable path, counted in
  `GetStats()`; a spec over a lossy transport.

## G -- lag compensation for hits (an amendment to ADRs 0076 and 0133)

- [ ] The authority keeps about a second of history of the bodies a script
  marks; a raycast or overlap "as player P saw it" goes back by P's
  interpolation delay and half its round trip, capped at 200 ms.

## H -- the rest of the network findings

- [x] N11: a long frame's warning says where its time went -- simulation and
  its scripts, render-step scripts, waiting for the GPU and the display, and
  the rest.
- [x] N3: what hears `Connected`, `Disconnected` and `JoinFailed`, in the
  multiplayer guide.
- [ ] N12 (the world replaced on join costs a frame of 80-180 ms, "the rest"
  in N11's split), N4 (`Enum.NetworkEndReason` beside the text), N6 (a "Start
  server" launcher in a `host` or `none` export).

## The audit's findings (NA1-NA34)

ludwerk-08's audit of 2026-10-02 against what professional engines do, with
the file and line of each and a failing test for each. **Measured** ones were
reproduced with real processes.

### P0 -- breaks play

- [x] **NA1** Joining a world of more than a few hundred parts: a snapshot
  over 16 KiB goes reliable in parts, held until acknowledged; acknowledgements
  checked; a refused send counted, not passed over. Ten thousand parts join in
  under three seconds over the loopback (was: 1 000 in 29 s, 5 000 never).
- [~] **NA2** The whole ground is encoded once for every peer. Its own channel
  and interest for the ground wait for a design: a chunk message ordered
  against the spawns it may name, and the chunks each peer holds.
- [x] **NA3** The input stream after long frames (D480, and the server's
  catch-up and intent budget in C).
- [x] **NA4**, [x] **NA5** -- see C.
- [x] **NA34** (from the kart game) Joints on the wire (protocol 34):
  `Attachment`, `Constraint` and its seven kinds and six movers, each with what
  its own properties hold, `Weld`, `WeldConstraint` and `NoCollisionConstraint`;
  references between instances beyond `Parent`; `Workspace.Gravity`. A replica
  solves a joint where it simulates one of its ends. Test: a hinge with a motor
  handed to a replica holds together there, turns at the motor's speed, keeps
  the owner's own change of speed, and the authority sees it turn.

### P1 -- visible, fragile or exploitable

- [x] **NA6** N7's doubled position: a take keeps no stale prediction.
- [x] **NA7**-**NA11** -- see E. [ ] **NA12**, [ ] **NA13** -- see C and E.
- [x] **NA14** The interpolation delay adapts to the link and is reported;
  the clock is nudged back. [ ] Bounded extrapolation past the newest sample.
- [x] **NA15** A move of over two metres a tick is drawn as a step.
- [~] **NA16** A correction is drawn whole on its first frame, then slides.
  [ ] Rotation smoothing; 2D smoothing and replay.
- [x] **NA17** A part a client's own scripts made is simulated by that client.
- [ ] **NA18** The own character collides with others where they were drawn.
- [x] **NA25** -- see D.
- [ ] **NA19** Send cost grows as players times instances; [ ] **NA20** no
  bandwidth budget, priority or quantisation -- both change what the snapshot
  checksum is over (ADR 0069), a design of their own.
- [x] **NA21** In order per remote. [x] **NA22**, **NA23** Budgets over time,
  and a flooding peer let go. [x] **NA24** An owner moves what it owns within
  reach -- **followed at the reach, never refused** since D482: a refusal was
  for good, and an owner whose kart began in the wrong place was held at the
  grid for the whole race.

### P2 -- gaps against professional practice

- [ ] **NA26** Clock synchronisation by time scaling. [ ] **NA27** `Loss` from
  sequence gaps, per direction. [x] **NA28** see E. [~] **NA29** see E; a
  stateless cookie against spoofed connects is not done. [ ] **NA30** the token
  in clear (with ADR 0120's build), any captured network id nameable, a player
  with no character sent the whole world. [ ] **NA31** attributes re-encoded
  every snapshot. [ ] **NA32** the replica's despawn, release and ack details.
  [x] **NA33** the name count is 32 bits.

### From the kart game (for after this ledger, unless they fit)

- [x] **K1** A ball, and a cylinder no longer than it is wide, spin up to
  500 rad/s; anything else keeps Jolt's 47 (a tumbling log at 500 tunnelled).
- [ ] **K2** A scene's scripts start before its streamed parts exist: an
  authority that serves others holding the whole world, or a "loaded around"
  signal -- an amendment to ADR 0053.
- [ ] **K3** Render-step callbacks in a headless run that draws for
  `--screenshot`. [ ] **K4** `HingeConstraint.CurrentAngle`,
  `PrismaticConstraint.CurrentPosition`.

### From the FPS game (2026-10-03)

ludwerk-08's findings from `GuerraDeTinta`, kept here until they have a ledger
of their own.

- [x] **K5** A part's `Material` written in a scene file is not there at run
  time: D483, the partitioned scene headed as version 1.
- [x] **K6**, **K7** Scene parts wearing some materials vanish once another
  material, or a mesh with its own, is in the world: D484, SDL's D3D12 backend
  keeping a single draw's vertex stride of zero for the instanced run after it
  (patch 0003).
- [ ] **G1** A placed stamp stores a world-space transform on every part, so a
  part moved inside the stamp file never moves in a copy placed away from the
  origin: the instance should store its root's transform, its children
  relative (a design change to ADR 0049).
- [ ] **G2** `"stamp": "barril"` in a scene is not found, where
  `Instance.stamp("barril")` is: the reader does not normalise the name.
- [ ] **G3** A connection made by a global module's function, called from a
  scene script, outlives the scene, where a thread it spawned does not.
- [ ] **G4** `Anchored` does not hold a `CharacterBody`.
- [ ] **G5** A first-person layer drawn over the world with its own field of
  view, lit by the world (a feature).
- [ ] **G6** A billboard label draws a box for a glyph, and fills the glyph
  cache; labels made before the cache clears draw garbage after it.
- [ ] **G7** No `ViewportFrame` draws when the workspace has no camera.
- [ ] **G8** `Enum.ScaleType` has `Stretch` and `Slice`; `Fit` and `Crop` are
  what an icon in a box wants.
- [ ] **G9** `Bone.Transform` moves nothing: a chest bone turned eighty degrees
  leaves the figure, and the hand bone after it, as they were.
- [ ] **G10** `HostFailed`'s reason carries its catalog key in front of the
  words; `JoinFailed`'s does not.
- [ ] The scene format's rotation is column by column, `m[column][row]`: say
  so in the format's documentation.

## Findings

- **The acceptance table, measured on the development machine** (2026-10-02,
  `netcode_acceptance`, a quiet run):

  | condition | corrections | stop (m) | ping (ms) | remote (ms) | interpolation (ms) |
  |---|---|---|---|---|---|
  | clean | 0 | 0.0000 | 1.0 | 16.5 | 66.7 |
  | 50ms | 0 | 0.0000 | 46.0 | 83.3 | 116.7 |
  | 150ms | 0 | 0.0000 | 150.0 | 166.7 | 116.7 |
  | 300ms | 0 | 0.0000 | 299.0 | 333.2 | 116.7 |
  | hang150 | 0 | 0.0000 | 48.0 | 66.6 | 166.7 |
  | hang400 | 1 | 0.0000 | 51.0 | 83.2 | 150.0 |

  Under the whole suite running beside it, `hang150` once took two: the bound
  for a long frame is two.
- **A remote's round trip is measured as a median.** With 2% loss one call in
  ten waits for a resend, and the mean of ten at 300 ms read 408 ms.
- **The transport's thread changed what silence means**: a frozen game still
  answers the transport, so the sessions count ten seconds without a message
  themselves -- on both sides.
- **The doubled position (N7) reproduced exactly as the owner measured it**:
  27.9 m put by the server read 55.8 on the replica.
- **D482 was NA1 and NA24 meeting.** A first snapshot that comes reliably in
  parts arrives after an ownership message sent the same tick, and the owner
  never placed the part it was handed; before NA24 the authority took the
  origin it was sent, and from NA24 it took nothing at all.
- **D481 was found by NA34's test**, not by a game: the owner's script read the
  gravity it booted with while the world beside it held the server's.
