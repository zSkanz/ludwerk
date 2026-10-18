# Changelog

Every release of Ludwerk. The format is [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the versions are [semantic](https://semver.org/spec/v2.0.0.html).

**The public surface is `api/api-dump.json`**, which is generated from the API
definitions and compared in continuous integration. A change to it is an entry
here.

## [Unreleased]

### Fixed

- A player walking on a moving platform, or standing on one that turns, is no
  longer corrected nearly every tick in an online game. Their machine draws
  the platform a few ticks in the past, and the correction stepped the player
  again from where the platform is now. Jumping off a moving platform is now
  corrected once at most, as the player lands, and landing on one at most
  twice.
- A player whose machine's clock runs fast is no longer corrected by
  however far they moved in the input the server dropped. Their input queued
  up at the server until the server caught up by dropping some of it, and a
  falling player then jumped 10 m. Their clock now runs a few percent slow
  until the queue is back to its length. The server still drops input past a
  ceiling, for a machine that stalled or floods, and logs it when it does.
- What a player predicts that moves while they touch nothing is no longer
  corrected by a tick of its own travel. The server held or skipped the
  ticks of a player sending no input to tune their queue -- harmless for one
  standing still, but a car rolling, a character falling, pushed or a crate
  sliding moved on by a tick the player's machine never stepped, and was
  corrected by exactly that, again at every adjustment. A player counts as at
  rest now only when nothing they predict moved since the last tick. And a
  car a script drives with nobody in it is drawn between snapshots rather
  than predicted on a machine that does not have its inputs.
- A braked car stands still. It was woken every tick its brake was held, so
  it never slept, and at walking pace the tyres' friction let it creep -- down
  a 10 degree slope at a centimetre or two a second for as long as it stood.
  A held brake no longer wakes it; the throttle, the wheel and any input that
  changes still do, so a car let off its brake on a hill rolls away.
- A button's first press in a match no longer reaches the server late now
  and then. An action was named to the server the first time it was pressed,
  and a lost packet held the name -- and the press -- back a round trip and
  more: on a link losing 2 % of its packets, a few drives in a hundred braked
  twelve ticks late on the server, and the driver's car was corrected three
  times by about 19 cm. Every action is named the first tick a script writes
  it, held or not.
- A car a machine begins to predict starts from the server's car whole: its
  wheels' suspension and the impulses their tires ended each step with now
  travel with it (protocol 56). A car born on a player's machine while it was
  still settling kept, before, a wheel resting where the server's was in the
  air. With `--net-log-corrections`, a correction of a car says which wheel
  was otherwise, and how.
- The editor tells you of a file changed outside it in the first half second
  after it began watching the project. Before, that change was taken for how
  the project began, and its tab kept the old text.
- A client in a match that stops for a moment -- a window dragged, a disk
  read, an alt-tab -- catches up the time it missed, up to a quarter of a
  second, instead of dropping it. Its car or character no longer jumps a metre
  or two ahead when it comes back. Two heavy frames in a row are clamped as
  before, so a slow machine does not spiral.
- A driven car's automatic gearbox shifts on the same ticks on the driver's
  machine as on the server: a car braking from speed is no longer corrected
  by 20 to 40 cm.
- A driven car on a slow machine at a high ping no longer drifts sideways and
  snaps back at a corner or a stop. A car a few milliradians off the server's
  heading was pulled back in place and speed at every answer, but never in
  heading, so it kept going sideways; it is now turned back too.
- A ray, a sphere cast and an overlap now meet characters. Before, a ray at a
  `CharacterBody` went straight through it, or answered with whichever other
  part happened to share its number in the physics world. `CanQuery` on a
  character is honoured. **This changes behaviour**: see the first entry
  under Changed.
- A player whose server presents a different key from the one remembered
  for that address is told so, with the address and what to do. Before,
  the join failed as "connection lost". `JoinFailed` gives the reason, and the
  command line logs it.
- `ludwerk test` counts a spec that fails to load as a failed case. Before,
  a spec with an error at its top level registered no cases, and the suite
  passed without it.
- A world a manager starts writes its log from the first line. The log
  folder was created after the world launched, so a new world logged
  nothing.
- A manager no longer lets more players into a world than it holds while
  some of them are still on their way: a player with a pass counts from the
  moment the pass is given, not from their first heartbeat.

- Long shadows from distant buildings no longer disappear at camera cascade
  boundaries. Each sun-shadow projection includes the depth of the objects
  that can shadow its visible region, rather than a fixed extrusion.
- A model whose mesh is still loading no longer flashes a thin white wire box
  in a game. A swapped character, a new piece of armour and a preview were the
  usual cases. With a camera, a part with nothing to draw yet draws nothing
  until it has something; the wire box is only for a world with no camera.
- A server takes more than eight players from one network address: a
  household, a LAN house, a school or a mobile carrier. The limit is
  `[network] max_connections_per_address` or `--max-per-address=N`, 64 by
  default, 0 for no limit. A player over it gets a `JoinFailed` that says why,
  where before they saw "connection lost". A flood from one address is still
  dropped, judged by how fast it connects.
- A player who joined while their character was still falling is no longer
  corrected by a couple of centimetres at the first answer: the ticks between
  the first intent the server answered and the first one the client predicted
  are stepped again too.
- A part larger than the streaming radius is sent to a player standing on it
  far from its centre. Interest is measured to the part's edge, so a 4 km floor
  no longer leaves the players on it falling through and corrected every tick.
- A script saved with a byte-order mark (Notepad, Windows PowerShell 5) checks
  clean in the editor and in `--check-scripts`, as it already ran in the game.
- A glTF node's morph weights are where its targets start, over its mesh's.
- A content file read in the middle of its author's save no longer drops
  what was loaded: a sound, a material, a viseme track or face map, a
  sequence, an animation graph and the roles beside a model keep what was
  read until a whole read succeeds, and a file left unreadable is said once,
  two seconds after its last change.
- A volume's light baked again, or its file changed while a development
  session runs, is the room's light at once, and only the file that changed
  is read again.
- The editor's Bake, on a machine whose settings work no volume out, works
  out the volume it was asked for and leaves every other its file's colour.
- In the Timeline, the name a `cast` track brings is in the list of bindings
  a track is added on, and a track on it is not marked unbound.
- Two parts welded together no longer push each other apart: a weld's two
  bodies do not collide, so a cabin welded on a chassis no longer drags it.

### Added

- Every match is encrypted, with nothing for a game to do: a dedicated server,
  a host and a test alike, and through a relay that cannot read what it
  carries. Each copy of a game keeps a key beside its saves, and
  `Player.PublicKey` is that key: the same player after the server restarts
  and on another server, which is what a ban or saved progress is kept by. A
  host's key is remembered the first time it is joined by address, and a
  later join that meets another key fails and says so;
  `NetworkService:ForgetServerKey` lets a game trust it again, and
  `[network] server_key` names the game's own server's key. `--insecure` and
  `[network] encrypt = false` turn it off for development, and a shipping build
  refuses both. `GetStats()` has `WireBytesSent` and `WireBytesReceived`, what
  the network itself carried. A build of this version cannot join an earlier
  one: the protocol is 54.
- A server says who may join: `NetworkService.OnJoinRequest` runs before a
  joiner is a player, may ask the game's backend, and lets them in or refuses
  them in the game's words. `NetworkService:VerifyTicket` checks a login
  ticket -- a standard JWT signed with Ed25519, which any backend's JWT library
  issues -- with no call out: signature, game, a life of at most 90 seconds,
  and used once. `Join(address, { Ticket = ticket })` presents one inside the
  encrypted connection. `Player.AccountId` is the account; the same account
  joining again drops the first; a ban of an account holds whatever key it
  comes back with. `ludwerk ticket keygen` and `sign` make a backend's key
  pair and a ticket.
- A dedicated server has a console on its standard input: `help`, `status`,
  `players`, `kick`, `ban` (by key, kept across restarts), `unban`, `say`,
  `save` and `stop [minutes]`, which counts down, saves, lets everybody go and
  ends cleanly. A game adds its own commands with `ConsoleService:Register`,
  typed parameters and a permission level, and runs one from a script with
  `ExecuteAsync`. What the operator says fires `NetworkService.Announced`, and
  `@engine/announcements` shows it across the top of the screen in one line.
- Online games of several worlds: `engine-host --manager=servers.toml`, or
  `ludwerk server run`, starts each world named in `servers.toml` from the
  server's own binary, starts a fallen one again within a limit, keeps each
  world's log, and answers players at one front door that sends each to the
  entry world. `update <folder>` moves every world to a new build a world at a
  time, its players carried across, nobody dropped. `ludwerk server install`
  makes it start with the machine: a systemd unit on Linux, a startup task on
  Windows.
- `NetworkService:Travel` and `TravelGroup` send players to another world of
  the game with what the game hands over, which `TravelArrived` reads there.
  Each hop carries a pass signed by the manager, good for thirty seconds and
  once. `TravelStarted` fires on the client before it leaves, and
  `@engine/travelscreen` is a loading screen over the hop in one line.
- `InstanceService:CreateInstanceAsync` makes a copy of a scene for one group
  -- a dungeon, an arena -- from `servers.toml`'s `[[instance]]` rows, a warm
  one at once. Only the group may enter, one who drops comes back to it, and
  it closes when it has been empty for its while. `InstanceClosed` tells the
  world that made it why it went; a group whose instance fell is sent back,
  and `TravelArrived` says so.
- `CrossWorldService:Publish` and `Subscribe` carry events between a game's
  worlds: a chat line, a boss that fell. `require("@std/net").websocket(url)`
  opens a WebSocket from a server, or from a client whose project allows it.
- A dedicated server or a manager can be administered from another machine,
  by key: `ludwerk server keygen` makes an administrator's key, `admins.toml`
  names who may come in, and `ludwerk server connect` opens the console --
  a prompt, one `--command` for a script, `--follow` for the log. Off unless
  `[server] admin` or `--admin` turns it on, and on the machine's own address
  unless `admin_allow` names others.
- `GET /health` and `GET /metrics`, as JSON, for a load balancer or a
  dashboard: `[server] health` or `--health` on a dedicated server, `[manager]
  health` for every world a manager runs.
- In the editor, a project with a `servers.toml` plays its worlds: Start match
  with *This game's worlds* runs the manager on the machine, each world
  without a window, with a Console tab for each world that also takes a line
  for its console, and a table in the Run view with *End*, which ends a world
  as a crash would, and *Restart with this build*.
- `examples/47-realms`: a town, the fields beyond it and a dungeon for each
  party that stands on the gate.
- The editor notices files changed outside it while it is open, such as by
  VS Code or another program:
  - A script it has not edited takes the new text, and the next Play runs it.
    A script edited in both places asks whether to keep your version or take
    the disk's.
  - New and deleted script files appear in or leave the Explorer.
  - The open scene, stamp or material is reloaded, or the editor asks first if
    you have edited it.
  - A save never writes over a file that changed on disk. It keeps the disk's
    version and puts yours in `.engine/conflicts/`.
  - While the game runs, changes wait until Stop. To restart Play with them
    instead, use the status bar or Preferences > Performance.
- Luau's flags are a project setting: `[luau.flags]` in `project.toml`, and
  Project Settings > Luau, which offers `if local` and exported values by name
  and every flag under Advanced. The game, the editor's checker,
  `ludwerk check` and VS Code read the same values. A player whose game has
  other values cannot join (protocol 53), and a replay recorded with other
  values is refused.
- `engine-host <project> --print-luau-flags` prints the values the tools that
  check scripts are given.
- In a sequence, `"$$..."` is a string that begins with `$` where a value
  goes; the writer puts the second `$` back.
- `assetc --cache <dir>`: a source is answered from what an earlier compile of
  the same bytes, name and options wrote there.
- Preferences, Performance: the editor's frame rate (the display's, 30 to 240,
  or unlimited), its slowing down in the background, the Viewport's quality
  while editing, and Play with the player's saved settings; the Viewport's
  quality is also a click away on its bar.
- Project Settings, Quality levels: the levels side by side, each cell the
  engine's value or the project's own, and a level for each platform. A
  level's own values are `[graphics.low]` to `[graphics.ultra]` in
  `project.toml`.

- Vehicles: a `VehicleController` on a chassis and a `VehicleWheel` for
  each wheel make a car that drives on a cast suspension, with an engine,
  gears and brakes. A player seated as its `Driver` drives it predicted, as a
  character walks, allowed three centimeters from the authority's car and
  pulled back gently, and drawn gliding when it is corrected. Protocol 52.
- `NetworkService:GetStats().PredictedDrift`: how far the parts this machine
  predicts were from the authority's at the last answer.
  `PredictedDriftPart` names the part that was furthest.
- `Player:GetNetworkPing()`: one player's round trip, in milliseconds -- on
  the server that player's, on a client its own. `GetStats().Ping` on a host
  is the worst client's link, which a host read as its own.
- `BasePart:Fracture`: a box part cut into pieces held by joints that break.
  A wall that stood keeps its footing and breaks where it is struck.
- `Cloth`: a skinned mesh that hangs as cloth, every vertex simulated, pinned
  by the joints `PinJoints` names, kept out of its body's `SpringCollider`s and
  the anchored parts near it, and blown by the wind. Picture only; off at the
  Low level and on a phone below High. `--skip=cloth` draws a run without it.

- A dedicated server spends its tick on players, not on drawing. It no
  longer builds a picture nobody sees. A player's interest comes from a grid
  instead of from every part, and what a player owns comes from the parts
  that have an owner. With 128 players and 1,000 server characters, telling
  the players what changed went from 133 to 86 ms a frame. A server that falls
  behind holds its snapshots while it catches up, and no longer spirals:
  under that load it now runs 38 to 40 frames a second, where it ran 1.8.
  `CaptureProfile` reports `calls`, `perCallMs` and `ticksPerFrame`.
- `ludwerk server loadtest`: a dedicated server is tried with light bots,
  one encrypted connection each, that walk, travel between worlds through
  pads tagged `Portal`, and run a behaviour script (`--bot-behavior`, with
  `require("@engine/bot")`). It reports a plateau and fails when a second
  runs over the tick, a bot fails or a message is dropped.
- A budget for each player: `[network] client_budget_kb_per_second`, 48 by
  default. What matters most to a player is sent first: their own character
  and what they own always, then what is nearest, times
  `BasePart.ReplicationPriority` and `Player:SetReplicationPriority`. What
  waits is drawn smoothly a little behind. `Player.ReplicationBudget` gives
  one player more or less. `GetStats()` adds `Deferred` and `BudgetUsed`.
- What no player is near sleeps on a dedicated server: `[server]
  dormant_radius`, `BasePart.Dormancy` and `Dormant`, and
  `Workspace:GetNearestPlayerDistance`. `@engine/npclod` calls an NPC's code
  less often the further it is from every player.
- An overloaded world slows its clock instead of stalling. With `[server]
  time_dilation` (or `--time-dilation`), a server that cannot keep up runs
  its ticks slower, to as low as `time_dilation_floor`, and every client
  slows with it; it returns to full speed when the load passes.
  `RunService.TimeDilation` and `TimeDilationChanged`.
- Channels: a world that fills opens another copy of itself, by
  `channels`, `max_channels` and `channel_open_at` in `servers.toml`.
  Channels that empty close, and ones that run low are merged.
  `NetworkService.Channel`, `SwitchChannel`, `TravelToPlayer`,
  `GetChannelsAsync` and `ChannelMerging`. `Travel` and its relatives skip a
  player who has left and say whom they sent. Saves are shared by every
  channel of a world, and two writers of one slot never mix.
- A queue at the front door: `[manager] max_players`, `queue_max` and
  `queue_grace_seconds`. A player who arrives when the game is full waits in
  line and is told their place and an estimate (`NetworkService.Queued`, and
  the travel screen). A player who drops keeps their place for the grace.
- Phases: `Instance.Phase` puts a thing in a phase, and only the players in
  it (`Player:AddPhase`, `RemovePhase`, `HasPhase`, `GetPhases`, `CanSee`)
  are sent it. A phased part collides only with the characters and owned
  parts of players who see it, in 3D and on the plane;
  `BasePart.PhaseCollision` turns that off.
- A shot is checked against the world its shooter saw:
  `Workspace:RaycastAsSeenBy` and `SpherecastAsSeenBy`, on the server, place
  every player's character and every `BasePart.LagCompensated` part where
  the shooter's machine drew them. The rewind is at most `[network]
  rewind_cap_ms`, 200 by default.
- What a player does past a limit is kept in one suspicion log: a remote
  called too often or too large, an action over the rate the game gave it
  (`NetworkService:DefineAction` and `CheckAction`), a part its owner put
  beyond its reach, a pass or a ticket only a forger sends, and what the game
  reports itself (`NetworkService:ReportSuspicion`). The server console's
  `suspects` and `suspect <player>` read it, and `/metrics` counts it by kind
  under `suspicion_total`. Nothing is done to a player by it, except that
  `[network] kick_over_limit = N` removes one whose calls go past the
  remotes' limits N times in a second.
- A game can be drawn in a headset on a PC, through OpenXR: `[xr] enabled =
  true` in `project.toml`, or `--xr`, tries a headset at start and draws each
  eye; with none -- no runtime, nothing plugged in -- the game is drawn on the
  screen as before. `--xr=simulated` draws into a headset that is the
  engine's own, for tests, and `--screenshot` then writes each eye's
  picture.
- In a headset, `XRService` tells a script where the head and the hands are
  (`GetHeadCFrame`, `GetHandCFrame`, `IsHandTracked`, `Enabled`), in the
  play space the camera places. The controllers' buttons, triggers, grips and
  sticks are `Enum.KeyCode` values beginning with `XR`, bound to
  `InputAction`s beside a gamepad's, for Meta Touch, Valve Index, HTC Vive and
  the simple controller; `Enum.InputDeviceType.XR` says one was used last, and
  `HapticService:SetMotor` vibrates one under `Enum.UserInputType.XRLeft` or
  `XRRight`.
- More of a headset reaches a script:
  - `XRService.Space` chooses a standing or a seated play space, and
    `Recenter` makes where the head is the origin.
  - `GetPlayAreaBounds` is the size of the room the player set up.
  - `SessionState` and `SessionStateChanged` say whether the player is in
    the game or in the headset's menu, and `UserCFrameChanged` fires as the
    head and hands move.
  - On a headset that tracks hands, `GetHandJoints` returns a hand's 26
    joints, a hand with no controller is held at its palm, and a pinch is
    the `XRPinchLeft` or `XRPinchRight` key. `[xr] hands = false` turns hands
    off.
  - `SimulatePose` and `SimulateInput` move the simulated headset's head,
    hands and controls from a test.
- In a headset, each `ScreenGui` is shown on a panel in front of the player
  (`XRDistance`, `XRPixelsPerMetre`, `XRFollow`), and the pointing hand
  (`XRService.PointerHand`) clicks its buttons, and those on any
  `SurfaceGui` or `BillboardGui`, with its trigger or a pinch, drawing a beam
  where it points. With hand tracking, a fingertip pushed into a button
  presses it. While a headset draws, the window shows its left eye, or
  nothing with `[xr] mirror = false`.
- A headset with cameras shows the player's real room behind the game:
  `XRService.Passthrough`, where `SupportsPassthrough` says it can, with
  `Lighting.XRPassthroughOpacity` to fade between the two.
- A game asks the headset for the player's room with
  `XRService:GetRoomAsync`: its floor, ceiling, walls and tables, each a
  rectangle of an `Enum.XRPlaneKind`. `XRService:CreateRoomParts` makes them
  solid, invisible parts. On the Quest it is the scene the player captured,
  and the player is asked first.
- A headset is given each eye's depth with its picture, where its runtime
  takes depth, so the picture it moves to where the head has turned holds
  steadier. `[xr] depth = false`, or `--xr-depth=off`, turns it off to
  compare.
- `ludwerk build --target=quest`, and the Export window's Meta Quest card,
  build a game with `[xr] enabled` as an APK that runs standalone on the
  Quest, starting in the headset. `examples/48-xr-room` is a room to try it
  in: things to pick up and throw, a teleport, and a panel of buttons.
- `@engine/xrcomfort` moves a player in a headset without making them sick:
  a teleport with an arc onto anything flat enough to stand on, snap or
  smooth turns, smooth movement with a vignette, a seated mode and height
  calibration.
- The server console's `mute <player|key> [minutes] [reason]` and `unmute`
  keep a list in `mutes.json` beside the bans, read by every world of the
  game; `NetworkService:IsMuted(player)` answers it, and what a mute stops is
  the game's.

### Changed

- **Behaviour change: a character on a moving platform rides it without
  lagging a tick**, in a single-player game too. A character standing on a
  part that a script moves was carried by the velocity the platform had a tick
  earlier, so it trailed the platform by one tick of travel. It is now carried
  by where the platform goes in the same tick. In a multiplayer game, a player
  riding a platform was corrected every tick of the ride; now they are not.
- **Behaviour change: a client's calls of one remote are limited.** On the
  server, each player may call one `RemoteEvent`, `UnreliableRemoteEvent` or
  `RemoteFunction` 240 times a second, with up to 480 at once, and send at
  most 16 KiB in one call; calls past it are dropped before any script sees
  them and kept as a suspicion. The same for a project made before this as
  for a new one. A game's own heavy channel lifts it with one line:
  `remote:SetRateLimit(0)` (or sets its own: `remote:SetRateLimit(600)`).
- A dedicated server steps its characters on several threads: those that
  nothing they could change is near -- no loose part, no other character that
  could reach the same place in the tick -- step at once, the rest one after
  another as before. The result is the same on any number of threads, and no
  two characters end up inside one another. `[server] physics_threads` sets
  how many threads beside its own a server's physics uses (four by default).
- **Behaviour change: rays meet characters.** `Workspace:Raycast`,
  `Spherecast`, `GetBodiesInBox` and `GetBodiesInSphere` now hit a
  `CharacterBody` as they hit any part. A cast that starts inside your own
  character, such as a ground check or a camera boom from its centre, now
  hits that character first. **To fix it, leave the character out**:
  `RaycastParams.new({ Filter = { character } })`, or set
  `character.CanQuery = false` if nothing should ever hit it. The engine's
  own casts already leave characters out where they should: the camera's line
  of sight, foot placement, detectors, particle collision, a crowd's ground and
  the light bake.
- Luau 0.742, luau-lsp 1.70.1, StyLua 2.6.0, SDL 3.4.18, fastgltf 0.9.1 with
  simdjson 4.6.11, meshoptimizer 1.3 and xxHash 0.8.4. Bytecode exported by
  the previous build still loads: the compiler still writes version 9, and the
  VM now reads versions 3 to 14.
- luau-lsp 1.70.1 carries Luau 0.740. In `ludwerk check` and VS Code it reports
  three errors on narrowed values that the editor's checker (Luau 0.742) does
  not. Write around them with a cast or a named function:
  - `Expected this to be 'string', but got '(string?) & ~nil'`, for a value
    checked against `nil`, then used;
  - `Expected type table, got '(Object?) & ~nil' instead`, for a table checked
    against `nil`, then indexed;
  - `Expected this to be 'number', but got 'string?'`, for a function written
    inside an `if` condition.
- Android games target API 36 (Android 16), which Google Play requires of new
  apps and updates. They still run from API 21. Building needs SDK platform 36
  and build-tools 36.0.0; `scripts/install-android.ps1` installs them.
- The wire protocol is 55. A build before this one cannot join a server of
  this one, or the other way round.
- In the editor, the scene is edited, and Play starts, in the project's
  graphics settings; a player's saved ones are Play's only when asked for.
- Models are compiled side by side, one to a worker, and give the same bytes
  as with one; the character example compiles in 3 s where it took 14.
- An export reads the editor's import cache: a model or a picture the editor
  already compiled is not compiled again, and the export says how many it was
  given. The cache's key now carries the mesh format, so every entry is
  compiled once more the first time.

## [0.0.1]

The first version under the name Ludwerk. What the engine has:

### Scripting

- Luau in strict mode, with generated type definitions and `luau-lsp` support.
- An `Instance` and `Service` API over a data-oriented core: instances,
  properties, attributes, tags, deferred signals, and a `task` scheduler.
- Modules as files, required by path, and code in three script services: one
  for the server, one for each client, one for the whole game.
- Hot reload: a saved script rebuilds the running world, and a saved asset is
  read again without restarting it.
- A script editor with completion, diagnostics and a debugger.

### Simulation

- A fixed-tick, deterministic simulation with interpolated rendering.
- Input recording and replay that reproduce a run exactly.
- 3D physics (Jolt) and 2D physics (Box2D): constraints, movers, queries,
  character bodies and ragdolls.
- Navigation: a navigation mesh built where it is asked for, agents, and
  swarms of many agents steered on open ground.

### Rendering

- Physically based forward rendering with cascaded shadows, clustered lights
  and image-based lighting.
- Reflection probes, irradiance volumes for bounced light, and reflections
  and bounce from the screen.
- Anti-aliasing (FXAA, SMAA, TAA), upscaling (FSR 1 and FSR 2) and frame
  generation.
- Post-processing, atmosphere and sky, particles, decals, beams and trails.
- Terrain with sculpting, materials and painting by rule, block worlds, water,
  foliage and wind.
- Materials as assets, and surface shaders written by hand or drawn as a node
  graph.
- Cameras that draw into textures, and worlds that run beside the main one.
- A 2D layer: 2D parts with sprites, sprite animation, tilemaps and joints.

### Animation

- Skeletal animation and animation graphs: states, blends, layers and events.
- Inverse kinematics, retargeting between skeletons, morph targets, and
  secondary motion for capes, tails and hair.
- A sequencer for cutscenes: cameras, animation, dialogue, sound and events on
  one timeline.

### Audio

- Positional audio with effects and mixing groups.
- Dialogue in several languages, with captions and lip sync.

### Interface

- A UI of frames, text, images, layouts, gradients and strokes, on the screen
  and in the world, driven by mouse, touch, keyboard or gamepad.
- Text and messages through a translation catalog, in the engine and in a game.

### Multiplayer

- One build that hosts, joins or runs as a dedicated server.
- Replication with client-side prediction and rollback, interest management
  and network ownership.
- Joining by address or by a code through a relay.
- Several local players at one machine, each with a part of the screen.

### Editor

- Explorer, properties, viewport manipulators and a content browser.
- Terrain, water and foliage tools; a timeline; a node editor for materials
  and animation graphs; a profiler.
- Play in the editor, with one player or a whole match.
- Export of a game for Windows, Linux and Android from one window.

### Tools

- `ludwerk`: `new`, `dev`, `edit`, `check`, `test`, `fmt` and `build`.
- `--log-keys`, and `ENG_LOG_KEYS=1` in the environment: every message of the
  engine is written with its catalog key before it, so that a script reading a
  log can match the key, which is the same in every language.

### Platforms

- Windows, Linux and Android. macOS is built in continuous integration.
