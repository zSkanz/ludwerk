# Multiplayer smoothness: why a player's character jitters, and the fix

Reported by the owner on 2026-09-28, after the first real match over Radmin VPN
(a dedicated server and a client on the owner's PC, a friend on another):
**both saw their own character "flick" while walking, worse when colliding with
something, and the friend saw the owner's character jitter too.** The owner's
rule: the multiplayer must not lag.

Measured with the owner's test game (`C:\Users\juanr\Downloads\Ludwerk-TesteMultiplayer`)
and an instrumented copy of its client that logs the character's position every
tick while a real key is held (walking in a straight line at 8 m/s, so every tick
should move exactly 8/60 = 0.133 m).

## What was measured

| Setup | Result |
|---|---|
| Headless client + dedicated server, same PC, virtual key | Irregular steps for ~0.2 s after walking starts (-0.119, -0.017, -0.04, +0.004, -0.15, -0.229 m...), then regular |
| **Windowed client + dedicated server, same PC, real key held** (the owner's setup) | 268 regular ticks, and **4 ticks that moved exactly two steps (-0.267 m)** in ~4.5 s of walking — each a visible pop forward |

That is with **zero network latency**. Over the internet (jitter, reordering,
loss) the same mechanism fires more often and with larger errors.

## Root causes

**1. The authority has no input buffer (the main cause).**
`engine/replication/src/session.cpp:895-946`: an `Intent` message REPLACES
`player->intents` and sets `peer->intentTick = tick`. Every authority tick, the
game reads `player:GetIntent` — i.e. the LATEST intent received, whatever tick it
belongs to.
- When no new intent arrives during an authority tick (client and server clocks
  are never phase-locked; a windowed client sends in frame bursts), the authority
  applies the previous intent **again** → the authority is one step ahead of the
  client's prediction for the acknowledged intent tick → correction forward (the
  measured -0.267 m ticks).
- When two intents arrive in one authority tick, the first is **never applied** →
  one step behind → correction backward.
- The snapshot's acknowledged intent tick (`session.cpp:1591`, `peer.intentTick`)
  is "the last intent received", not "the last intent whose step is in this
  position", so the reconciliation compares against the wrong prediction sample
  whenever arrival and application differ.
- The same irregularity is what OTHER players see: the authority moves the
  character irregularly, and that motion is what replicates.

**2. Intents are sent once, on an unreliable channel, with no redundancy.**
`wire_schema.gen.h:360` (`Intent`, `UnreliableSequenced`): a lost packet is a lost
tick of input → same as cause 1.

**3. Corrections snap.** `session.cpp:2490-2530`: past 1 cm the character is put
where the authority said and replayed; nothing smooths the visual result, so
every correction is a one-frame pop. Engines that predict (Unreal's character
movement, Source) keep the simulation corrected but decay the VISUAL error over
~100 ms.

**4. Collisions make it worse by design.** A replica simulates only its own
character (ADR 0076 §5); everything else it collides with — loose parts, other
players' characters — is drawn from snapshots **4 ticks behind**
(`types.h:116`, `interpolationDelayTicks = 4`). The authority collides against
where those things ARE. Every contact is therefore a guaranteed mismatch and a
correction.

**5. Nobody can see it.** `m_stats.corrections` (and `replays`, snapshots, bytes)
exists in `replication/include/engine/replication/types.h:156-174` but is shown
nowhere: not in F3, not in the log, not to scripts. There is no ping, jitter or
loss figure either. Diagnosing this took an instrumented build.

## The fix (in this order)

1. **Input buffer on the authority.**
   - Each peer's intents are queued by intent tick.
   - The authority applies exactly ONE intent per peer per tick, in tick order: it
     consumes the next one, holds the last one only when the queue is empty (and
     counts that as a starvation), and drops excess beyond a small cap to catch up.
   - A short target delay (1–2 ticks, adaptive to measured jitter) keeps the
     queue from running dry.
   - The snapshot acknowledges **the intent tick whose step produced this
     position**, not the last received.
   - `player:GetIntent` returns the intent being applied this tick.
2. **Redundant intents.** Each intent packet carries the last N (3–4) intents, so
   one lost packet loses nothing; the authority de-duplicates by tick.
3. **Visual smoothing of corrections.** Keep the corrected simulation state, but
   render the own character with an offset equal to the correction that decays
   over ~100 ms (and snap only past a large threshold, e.g. 2 m — a teleport).
4. **Collisions against interpolated objects.**
   - For the predicting replica, other players' characters should not block its
     own character in prediction (or should be placed at their extrapolated
     present position).
   - Loose parts near the own character should be extrapolated to the present
     rather than drawn 4 ticks late for collision purposes (the rendering can stay
     interpolated).
   - Measure which of the two causes the collision pops before choosing.
5. **Network stats, visible.** An F3 **Network** panel: ping (RTT), jitter,
   packet loss, snapshots/s, corrections/s and the size of the last correction,
   input-buffer depth and starvations. The same figures reachable from a script
   (a `NetworkService:GetStats()` table), so a game can show a connection-quality
   indicator.

## How to verify

- A replication test with an artificial transport that adds jitter (±20 ms),
  reordering and 2% loss, where a replica walks a straight line for 10 s:
  **zero corrections** (or corrections below 1 cm) with the fix; count them before
  the fix to show the test catches it.
- The instrumented client above: in the owner's setup (windowed client +
  dedicated server on one PC, real key held), every walking tick moves exactly
  8/60 m.
- A replica pushing a loose crate and walking into another player: report
  corrections per second before and after.
- Then the owner's real test over Radmin.

## Notes from the measuring session

- **A windowed client ignores `InputService:SetVirtualState`-driven movement**
  in this build (the character did not move with a virtual key held in a
  focused window; a real key worked). Headless, the same virtual key moves the
  character. Check whether that is intended.
- A second copy of the dedicated server started on a busy port logs
  `net.err.transport_open_failed` and then **keeps running**, a server that
  listens nowhere; a client then silently joins whatever already listens on that
  port. A dedicated server whose transport cannot open should exit with a
  non-zero code (and say so in a window-less message a person will see).
