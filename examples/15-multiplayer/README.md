# 15-multiplayer

Two copies of one game, one of them deciding (ADR 0069 and ADR 0070).

```
run.bat --host                  # a window, and the authority
run.bat --join=127.0.0.1        # a second window, showing the host's world
run.bat --serve                 # the authority with no window
run.bat                         # solo: the same world, nobody networked
```

`--host` and `--serve` take a port (`--host=7777`, the default); `--join`
takes an address and an optional port (`--join=192.168.0.10:7777`).

## What it shows

- **One project, every posture.** The script asks `NetworkService.Authority` —
  a gameplay question, "do I decide this world?" — and builds and animates the
  world only when the answer is yes. Solo answers yes too, so running it with no
  flag at all is the same game a host plays.
- **A replica builds nothing.** The ground, the ring of crates and the turning
  bar arrive from the authority: names, colours, sizes and motion, as a diff
  against the last state the replica acknowledged. The crates fall on the
  authority; the replica's copies are driven, never simulated, so they land
  where the authority's did.
- **No script opens a port.** The posture is chosen on the command line, before
  any script exists, and `NetworkService` only reports it.

- **Every player drives a racer with WASD.** A replica's keys travel to the
  authority as *intent* — the value of its `Move` action, never a position —
  and the authority moves that player's racer. `player:GetIntent("Move")` reads
  the player at this machine and a remote one the same way, which is why the
  same loop drives one racer solo and one per window when hosting.

- **A replica's own racer moves at once.** The authority names each player's
  racer as their `Player.Character`; the replica drives its own from its own
  keys the moment they are pressed, as the authority will when the intent
  arrives, and the authority's snapshots correct it rather than overwrite it
  (ADR 0076). Everyone else's racer, the crates and the spinner are drawn a few
  ticks behind, between two snapshots, so they glide instead of stepping.
- **Each replica is sent what is near its racer**, out to
  `StreamingService.LoadRadius`, and nothing else.
- **G seats a second player at the same machine** (arrow keys, or a second
  controller). `AddLocalPlayer` answers a `Player`, the authority gives it a
  racer like anybody's, and a machine that joined predicts **both** its
  racers -- one loop over `GetLocalPlayers`, the same `Racing.drive` the
  server calls. Each answers its own keys at once and is corrected by
  itself (ADR 0208). `--net-delay=150` on the joiner makes the difference
  easy to see: without prediction the guest's racer would start a third of
  a second after its key.

## What it does not show yet

The transport is ENet, which is **unencrypted and unauthenticated**: a LAN or an
otherwise trusted link.
