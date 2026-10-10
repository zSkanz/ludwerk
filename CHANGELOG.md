# Changelog

Every release of Ludwerk. The format is [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the versions are [semantic](https://semver.org/spec/v2.0.0.html).

**The public surface is `api/api-dump.json`**, which is generated from the IDL
and diff-checked by CI. Anything that changes it is an entry here; anything that
does not is engine work and belongs in the git history rather than in this file.

## [Unreleased]

**The first version as Ludwerk, 0.0.1** (ADR 0109). The engine's name, the command and a
project's files change, with no compatibility for the old ones: rename a project made before
by hand -- `luaug.toml` to `project.toml`, `.luaug/` to `.engine/` (or delete it; it is a cache),
`require("@luaug/...")` to `require("@engine/...")`, a surface shader's
`#include "luaug/surface.hlsli"` and `LUAUG_*` macros to `engine/surface.hlsli` and `ENG_*`,
and any `LUAUG_*` environment variable to `ENG_*`. The command is `ludwerk`.


### Changed -- BREAKING

- **What a client-side script makes is its machine's alone, on a host too**
  (ADR 0186). An instance made with `Instance.new`, `Instance.stamp` or
  `Clone` by a script that does not run on the server side -- the client
  script services, `RunContext = Client`, and a script every machine runs --
  is never sent to another machine, with everything under it. A machine that
  joined always worked this way; a machine hosting a match sent its friends
  everything its own client code made. **If a host's client code made
  something a friend is meant to see, move that to `src/server/`.** A game's
  client-side visuals no longer need to hide under a `Camera`. The wire is
  unchanged.

- **The wire protocol is 43** (D576): a replica's acknowledgement of a
  snapshot is sent without a guarantee, on the state channel, where it was
  reliable and on the control channel in front of every `RemoteEvent` a
  client fires. Every machine in a match must run a build that speaks it.
  What it bought: on a link that loses packets, a client's remote calls are
  no longer held behind a lost acknowledgement until it is sent again -- at
  300 ms and 2% loss, one call in five arrived a tenth to a third of a
  second late, and now the ones that are late are the ones the link lost.
  Nothing in the API changed for it.

- **The wire protocol is 42**: a snapshot's checksum is computed another way
  (ADR 0069, amended), and nothing else on the wire changed. Every machine in
  a match must run a build that speaks it. What it bought: a host's send
  costs what changed and not what there is -- 6.4 ms a tick down to 1.8 for a
  world of 2,700 replicated instances and one friend, with nothing allocated
  an instance.

- **The wire protocol is 41** (D570): `Swarm.Enabled` replicates. Every
  machine in a match must run a build that speaks it: one that does not is
  refused at the join, by name. Nothing in the API changed for it.

- **An exported game is one sealed pack** (ADR 0183), where it was a folder of
  its scripts, its catalogues, `project.toml` and a manifest of every asset's
  path beside the content pack. `ludwerk build` makes it for every target;
  nothing in a game changes, and `[export] packed = false` makes the folder
  of files as before. A tool or a script of your own that read files out of
  `game/` reads them out of the pack now: `assetc unseal <game> --out
  <folder>` writes them back.

- **Protocol 40** (ADR 0166, ADR 0167). What it carries less of: an
  instance's attributes and tags are sent as what changed, not whole; a
  rotation is eight bytes where it was thirty-six, and is not sent at all for
  a body that moves without turning; a client's held input is said once a
  message. A friend in a run of the horde test game received 41 KB a second
  and now receives 16 to 17, and sends 5.4 where it sent 7.9.

- **`GraphicsService.RenderScale` goes down to a third** (ADR 0164), where it
  stopped at a half: FSR 2's furthest mode. A scale written below a half was
  read back as a half and is now read back as written. The settings screen's
  steps are a third, 50%, 59%, 67%, 80%, 90% and 100%.

- **Scene files are version 3** (ADR 0155): a stamp's nodes carry sids, a
  copy's overrides are keyed by them, and a copy writes where it stands with
  its parts in its stamp's frame. A version 2 scene opens and converts; a build
  before this one cannot open a version 3 file.
- **A copy of a stamp no longer unlinks** when a child is added to it or one of
  its stamp's is removed: the child is the copy's own, or the stamp's child is
  disabled in it, and the copy keeps following its stamp. A stamp may hold
  copies of other stamps; one that holds a copy of itself is refused.
- **The wire is protocol 39**: `BasePart.ReceivesDecals` travels with a part.
- **The wire is protocol 38** (ADR 0161): the channel and the two messages of
  `UnreliableRemoteEvent`. A client and a server of different builds refuse
  each other, and say so.
- **The wire is protocol 36**: a client and a server of different builds refuse
  each other -- and now say so (`JoinFailed`, "another version of the game").
- **Teams are withdrawn** (ADR 0099, amended by the owner): `TeamService`,
  `Team`, `Player.Team` and `AutoAssign` are gone. A side is the game's own
  state -- `player:SetAttribute("Team", "Red")` on the authority, which every
  machine reads. Network ownership is unchanged.
- **`ProximityPrompt`, `ProximityPromptService` and `DragDetector` are
  withdrawn** (ADR 0126, amended by the owner), with their enums
  (`ProximityPromptStyle`, `ProximityPromptExclusivity`,
  `ProximityPromptInputType`, `DragDetectorDragStyle`,
  `DragDetectorResponseStyle`): a prompt and a world drag are the game's own
  code, and `examples/30-interactions` writes both in Luau. `ClickDetector`
  and `UIDragDetector` stay. A scene that holds one of the withdrawn classes
  loads without it. Text chat (ADR 0132) will not be built: a game's chat is
  its own code on `RemoteEvent`.
- **A locked pointer reaches no part of the interface** (D490): nothing under
  it is hovered, pressed or activated; a game that clicked a button with the
  pointer locked frees it first.
- **A property read through `game`, `workspace` or `script` is read when it
  runs** (D481); it was the value the script started with.
- **A join changes nothing until the server takes it** (ADR 0106, amended): a
  game's scene, its menu and its own server code run on while `State` is
  `Connecting`, and the world becomes the server's at the welcome. A game that
  counted on the scene being cleared at `Join` sees it cleared a moment later.
- **Leaving a match returns to the scene the join was made from**, and the
  local player is `UserId` 1 again; it used to stay in the server's scene, as
  its authority.
- **A dropped connection being dialled again is `Reconnecting`**, a new
  `Enum.NetworkState` item, where it was `Connecting`.

- **`BasePart.LinearVelocity` and `AngularVelocity` can be written** (ADR
  0127): an assignment used to raise, and now sets the speed the next tick
  starts from. A script that relied on the error to mean "read-only" no longer
  gets one.
- **A sprite is drawn in the colours it was painted in** (ADR 0153, D464):
  `Part2D.ExactColor` and `Tilemap2D.ExactColor`, on by default, take a sprite
  past the scene's exposure, tone curve, colour correction and bloom, so
  `Color3.fromRGB(200, 80, 40)` is that pixel. A 2D game's colours change --
  towards what its scripts say; set `ExactColor = false` for a sprite that
  should be lit with a 3D scene. The wire protocol is 32.
- **An Android debug build is signed with Android's own debug key** (D465),
  `~/.android/debug.keystore`, not one under the engine's folder. **A game
  already on a phone was signed with the old key and will not update**:
  uninstall it from the phone once -- which deletes what it saved there -- and
  install again. `[export.android] debug_keystore` names a key a team shares.
- **What takes a press is what does something with it** (`UIObject.Active`,
  D452): a `TextLabel`, an `ImageLabel` and a `Frame` with no background no
  longer take the pointer, so a press on them goes to what is under them -- a
  button's own icon, the screen below, the game. A panel made of an
  `ImageLabel`, or an invisible blocker, sets `Active = true`.
- **A scene change resets `Workspace` itself** (D450): its properties, its
  attributes and its tags, as every other scene-scoped service's already were.
- **A leaving player is out of `GetPlayers` at once and readable in
  `PlayerRemoving`** (D459), and a destroyed part is out of every query from
  the moment of `Destroy` (D458).
- **The overlay's key is not delivered to the game** in a host that has an
  overlay (D461); `[debug] overlay_key` moves it.
- **The wire protocol is 31** (D460): `Lighting.ExposureMin` and `ExposureMax`
  travel.
- **A screen the game's own code made is named when a scene takes it**
  (D469): a warning, once, saying to set `KeepOnSceneLoad`.
- **Something made where another thing was just destroyed is not shoved by
  it** (D467): a character respawned at the place of death came out to one
  side, or lost a jump.
- **The documentation site builds again** (D463): it had written nothing since
  the rename, and the gate that builds it now looks for the page as well as
  the exit code.

- **`CharacterBody:Move` is a direction and a throttle** (D440): its length is
  clamped to 1, so `Move(1, 0, 1)` walks at `WalkSpeed` and not 1.41 times it.
  A game that normalised the vector itself changes nothing; one that passed a
  longer vector to go faster raises `WalkSpeed` instead.
- **`WalkSpeed` is the horizontal speed on a slope too** (D439): uphill is
  faster than it was and downhill is the same across the ground. A game that
  compensated -- scaling `WalkSpeed` uphill -- removes that.
- **`CharacterBody.LinearVelocity` is the velocity the step produced** (D441),
  not the one asked for: zero against a wall.
- **`AnimationTrack.Weight` keeps what the script set** (D438): a fade no
  longer changes it, and after `Stop` it reads what it read before.
- **A scene's client scripts start again when the world is replaced** (D433):
  `ClientScriptService`'s scripts start fresh after a join succeeds or fails
  and after a match ends, and do not run while a join is being asked for.
  State that has to live across a join belongs in `GlobalScriptService.Client`.
- **`NetworkService.Connected` fires on every return to a match** (D432), and
  a connection that drops is `Connecting` until it is back or
  `[network] timeout` has passed, then `Disconnected`.
- **A project run by hand saves into the project** (D445):
  `engine-host <project>` writes `.engine/saves/`, like the editor's Play, and
  only the exported game writes the player's folder.
- **`UIService.SafeAreaInsets` on a phone is the cutout and the visible bars**
  (D447), not the system's gesture strips: a layout that relied on the old
  margin at the sides has none now.

- **A cell of streamed ground somebody changed is not kept in memory for
  ever** (ADR 0149, ADR 0150): outside the editor and a match, once more than
  256 MiB of changed ground is held past the load radius (64 MiB on a phone),
  the furthest of it goes to the session cache when no camera, focus or
  loose body is near, and comes back changed.
  A game that counted on changed ground far from every camera being there --
  a raycast into it from a script loads it first, as before; a body that
  walks onto it from elsewhere does not -- adds a `StreamingService` focus.
- **A `Script` in `ServerStorage` or `ReplicatedStorage` does not run**
  (ADR 0137 §3, D341). Storage holds templates and modules, and a template
  must not run itself; a scene that has one there says so in the log when it
  loads. Move it into the world or a script service, or clone it from storage
  into the world at run time.
- **A script runs exactly while it is live** (ADR 0137, D339, D340): a clone
  parented into the world, a model carrying scripts and a stamp placed with
  `Instance.stamp` start their scripts, and a script that leaves the world --
  parented to nil, into storage, or with the model holding it -- stops. A game
  that counted on a cloned script staying dead, or on a removed one going on,
  changes.
- **A script can read `Source` and cannot write it** (ADR 0137 §4, D332).
  `Script.Source` and `ModuleScript.Source` are the editor's to write: a
  script that could write another's code and enable it ran text the sandbox
  never loaded. A script made at run time takes its code from a copy -- `Clone`
  one in the scene, or `Instance.stamp`. The IDL mark is `ScriptReadOnly`.
- **A package's scene, stamp and `global.json` scripts are compiled** (D333),
  as `src/` already was, unless `[export] ship_source = true`: their `Source`
  is `luauc:` and the bytecode in base64 in the packaged file.

- **`SceneService.CurrentScene` is a `Scene`, not a path** (ADR 0124). Read
  `SceneService.CurrentScene.Path` where the path was compared.
- **A scene's scripts' close handlers go with the scene** (ADR 0124). A
  `game:BindToClose` registered by a scene's script is dropped when that scene
  closes, without running, and the editor warns; `scene:BindToClose` is the one
  for a level. The editor's Stop now runs the game's close handlers.

- **`luaug build` writes `dist/windows/`**, not `dist/win64/`, and the zip
  beside it. `--target=win64` still works. `scripts/android-player.ps1` is a
  wrapper around `luaug build --target=android`; its APK is
  `dist/android/<name>-<version>.apk` in the project.
- **The `player` profile is built with networking** (`LUAUG_ENABLE_REPLICATION`),
  so an exported game can host, join and serve; before, every exported game was
  single-player whatever its scripts asked for.

- **Code lives in three script services, and `ScriptService` is retired** (ADR
  0105). **Where a script is decides which machine runs it**:
  `ServerScriptService` (the scene's) and `GlobalScriptService.Server` run only
  where the world is decided -- solo, a host, a dedicated server -- and never
  reach a client that joined; `ClientScriptService` and
  `GlobalScriptService.Client` run where a player sits, never on a dedicated
  server; a script anywhere else runs everywhere, as before. Solo and a host run
  both, so a single-player game plays as it did.
  - **The moves.** `src/scripts/` is `src/client/` (read under its old name for
    this release, with a warning). The rules of a multiplayer game that sat
    behind `if NetworkService.Authority` go to `src/server/` or the scene's
    `ServerScriptService`. Modules both sides require go to `src/shared/`, which
    now mounts as `ModuleScript`s under `GlobalScriptService.Shared` -- one
    module whether required by path or by instance. A scene's own code goes in
    `src/scenes/<scene>/server/` and `client/`.
  - `game:GetService("ScriptService")` raises a keyed error naming the two new
    homes. A scene that saved something in `ScriptService` reads it into
    `ClientScriptService`.
  - `GlobalScriptService`'s folders `Server`, `Client` and `Shared` are made by
    the engine and cannot be renamed, moved or destroyed. What is authored under
    it that is not code is saved in `content/global.json`, not in a scene.
  - A script made in the editor inside one of the three services is written as a
    file under `src/`.
  - Every determinism trace was re-recorded: the world hash covers every
    instance, and the data model's services changed. The simulation did not.

### Added

- **`SpringBone` and `SpringCollider`: capes, tails and hair** (ADR 0194).
  A `SpringBone` under a `MeshPart` names a joint of its rig, and the joints
  below it trail behind the body -- swinging on a turn, streaming on a run,
  settling on a stop -- on top of whatever clip is playing, kept out of the
  body by `SpringCollider`s on its joints and pushed by the workspace's wind.
  `JointPattern` takes a whole cape in one instance (`Cape_*0` is the top of
  each column). The model needs only joints; nothing is re-imported. It is picture only:
  each machine's own, not replicated, not in a replay, and a `Bone` on such a
  joint still follows the clip. See `examples/35-cape` and the manual's
  "Capes, tails and hair".
- **Parts that are see-through are drawn in runs.** Blended parts of one
  mesh and one look that stand together in the blended order are one call,
  as solid ones have been -- each with its own colour and its own
  `Transparency`, so the pieces of an effect at different ages are one run.
  The picture is the same to the pixel: a run draws its members in the order
  they were drawn in one at a time. A part thinned by `Fade`, a skinned one
  and a surface shader's material are still a call each.
- **The frame report says what the game holds.** A third line: the process's
  memory against the machine's, the bytes and count of the textures and of
  the buffers on the graphics card, and the scripts' heap.
- **The frame report says what its draws were.** A second line follows each
  report (`--frame-report=N`, `[debug] frame_report_seconds`): the frame's draw calls
  by pass and kind -- `sun_shadow`, `local_shadow`, `prepass`, `decal_mask`,
  `mesh` (one object a call), `mesh_run` (an instanced run), `blended`,
  `blended_run`, `outline`, `highlight`, `velocity`, `foliage`, `decal`, `sprite`,
  `ribbon`, `particle`, `world_ui` -- which add up to the report's total.
  Kinds that drew nothing are left out.
- **`--gpu=NAME` chooses the graphics API a run draws through** (D596):
  `vulkan`, `direct3d12` or `metal`, on the engine's host and on an exported
  game alike. A name no device can be made through warns and takes the
  system's choice, so a shortcut carrying it cannot leave a game unable to
  start.
- **`RunService:GetLastRun()`** and `Enum.RunOutcome` (ADR 0187): how the run
  before this one ended on this machine -- `Clean`, `Crashed`, `Unfinished`
  (ended from outside, which a phone does to a game in the background) or
  `None` -- the count of script errors nothing caught and the first of them,
  and the path of a one-file report written for a run that left something to
  report: what the game and the engine were, the crash note, the end of that
  run's log. For a shipped game to tell its player the last session did not
  end well and which file to send. Nothing is uploaded and nothing is shown.
- **An Android release export makes an App Bundle beside the APK** (ADR 0104,
  amended): `dist/android/<name>-<version>.aab`, the file a store takes. The
  engine is the bundle's base and the game an install-time asset pack in it,
  delivered with the app. `[export.android] bundle` turns it on for a debug
  export or off for a release. Signed with the same key and checked the same
  way; install it from your store's testing track before you publish.
- **`[export.android] internet` and `vibrate`**: whether the package declares
  the network permissions and the vibrator's. Left out, each is declared when
  the project's scripts use what needs it.
- **`NetworkService:PingRelayAsync(relay?, timeout?)`** (ADR 0178, amended):
  asks a relay how far it is and what it carries, before hosting or joining
  through it -- what a game with relays in several regions calls on each to
  choose one. Yields; returns `{ Ping, Matches, Relayed, BytesPerSecond,
  Uptime }`, or `nil` where the relay did not answer.
- **`@std/json` in the game VM**: `json.serialize`, `json.deserialize`,
  `json.null`, `json.object`, `json.asObject` and `json.asArray`, Lute's
  names. A project no longer carries its own encoder to talk to a service.
- **A terrain layer glows** (ADR 0113, amended): a layer whose material has
  an `Emissive` colour gives that light off -- lava, a rune ring -- on every
  quality level and on a phone. Not `EmissiveMap`.
- **`GraphicsService.FoliageDecals`** (ADR 0185, amended), `[graphics]
  foliage_decals` and `--[no-]foliage-decals`: whether a decal paints the
  foliage standing in it. **Off at the low quality level, and on a handheld
  below high** -- there a decal is on the ground under the grass, as it was
  before decals painted foliage. A game that wants it on a phone sets
  `foliage_decals = true` under `[graphics.android]`.
- **`Instance.Local`** (ADR 0186), read-only: whether the instance was made by
  a script that does not run on the server side, and so is never sent.
- **`--net-log-client-writes`**: on a machine that is hosting, a client-side
  script's write to something every machine has is everybody's. This logs each
  such write once a script and property -- properties, `SetMaterialParameter`,
  `SetAttribute`, `AddTag` -- with the script's name.
- **A decal paints foliage, and `FoliageLayer.ReceivesDecals`** (ADR 0185). A
  mark laid on a meadow was drawn on the dirt and left every blade of grass in
  it clean; it is on the grass now, in all three blend modes, with nothing to
  change in a game. `ReceivesDecals = false` on a layer keeps what it grows
  clear of every mark, as the property of the same name does for a part.
- **`BasePart.Fade`** (ADR 0184): how much of a part THIS machine leaves out
  of its own picture, 0 to 1. Never sent to another machine and never saved,
  so it is what changes one player's view of a part every player has -- the
  tree between a camera and its hero thinned on that screen alone, where
  writing `Transparency` on a host thinned it for everybody. A part thinned
  this way keeps the shadow its `Transparency` gives it.
- **`Swarm.PileHeight`** (ADR 0156, amended): how high over the ground a crowd
  may pile on itself, in metres; 0, the default, is no limit. A crowd round
  something that stood still became a tower over it.

- **The GPU's time, pass by pass** (ADR 0171). `[debug] gpu_pass_times = true`
  in `project.toml`, or `--gpu-pass-times`, adds a line under the frame
  report: what each pass of the renderer took of the GPU, a frame's mean, the
  costliest first. For a phone, which has no profiler attached. It is a
  measuring mode -- the frame is stopped after every pass, so its frame rate
  is not the game's; the picture is unchanged.
- **Keys that take things out of a frame, to see what they cost** (ADR 0171):
  `[debug] hide = "foliage,terrain"` (also `parts`, `skinned`, `voxels`,
  `transparent`, `particles`, `ribbons`, `decals`, `sprites`, `world_ui`,
  `ui`, `lights`, `highlights`), `[debug] shadow_taps = 4`, and their flags
  `--hide=` and `--shadow-taps=`. Every frame report says which are in force.
- **`InputService:SetClipboard(text)`** (ADR 0177): a script puts text on the
  player's clipboard -- a room's code copied at a click. There is no way to
  read it.
- **A clip plays inside a `ViewportFrame`** (ADR 0107, amended): an
  `AnimationPlayer` under a skinned mesh in a frame animates it. It stood in
  the clip's first pose for ever.
- **`[debug] skip = "shadow,environment"`** and `--skip=` (ADR 0171): the
  terms of a lit surface left out of every frame -- `sun`, `shadow`,
  `contact`, `lights`, `environment`, `ambient`, `occlusion`, `fog`,
  `normal_map`, `material_maps`, or `unlit` -- to see what each costs.
- **An Android build can be handed the host's flags at launch** (ADR 0171):
  with `[debug] launch_arguments = true` in `project.toml`,
  `adb shell am start -n <id>/engine.player.PlayerActivity --es args "--gpu-pass-times --hide=ui"`
  runs the installed game with them. Without the key they are ignored.
  `--render-cap=N` is the new flag of `[graphics] render_cap`.
- **`[debug] log_ui_touches = true`** (ADR 0171): a log line for each finger
  that comes down, naming the element of the interface that took it and its
  rectangle, or saying the finger is the game's.
- **A phone's back button is `Enum.KeyCode.Escape`** (ADR 0170). On Android
  the back button and gesture closed the game; they are now delivered to it,
  as the key a desktop game already answers. A game leaves with
  `game:Shutdown()`.
- **`NetworkService.MaxPlayers` and `Player:Kick`** (ADR 0167): a host says
  how many players its match takes, its own among them, and removes one --
  who is told why in `Disconnected`. Both from a script, on the authority.
- **`NetworkService:GetStats()` says where the bytes go** (ADR 0166):
  `SnapshotBytes`, `AttributeBytes`, `RemoteBytes`, `UnreliableBytes` and
  `InputBytes`, beside `SwarmBytes`.
- **`TextLabel.TextOverflow`** (ADR 0168), an `Enum.TextOverflow`: text its
  box does not hold is drawn past it (`Overflow`, as before), cut at it
  (`Clip`), or cut short and ended with an ellipsis (`Ellipsis`), measured
  with the label's own font.
- **`Swarm:GetAgents(into?)` and `Swarm:GetPositions(into?)`**: into a table
  of the caller's own, as `QueryRadius` already took one, so a horde read
  every tick makes no table.

- **Frame generation** (ADR 0165): `GraphicsService.FrameGeneration`,
  `frame_generation = true` under `[display]` and `--frame-generation`. A frame is made between
  every two the world is drawn -- AMD FSR 3's -- so twice the frames are
  shown for the same drawing. The interface is drawn on every frame shown and
  is never interpolated. Each drawn frame is shown half a frame later than it
  would have been, and the display is waited for while it is on. With any
  anti-aliasing and any upscaling; the world's main camera with perspective,
  on a device with compute shaders, not on Metal. One of the display's
  settings: no level turns it on or off.

- **FSR 2** (ADR 0164): `Enum.UpscalingMode.FSR2`, `upscaling = "fsr2"` and
  `--upscaling=fsr2`. The window's picture is built from the frames before
  this one, each drawn a fraction of a pixel off from the last, so what half
  the resolution cannot hold in one frame is put back; it is the
  anti-aliasing as well, and at a `RenderScale` of 1 that alone. Exposure,
  bloom and the tonemap read the upscaled picture. Particles, glass and
  labels in the world are kept out of its history. The world's main camera
  with perspective, on a device with compute shaders; FSR 1 upscales anywhere
  else. No preset turns it on.

- **`BasePart.ReceivesDecals`** (ADR 0160, amended): off, no decal paints the
  part -- a character standing on a scorch mark, an enemy inside a warning
  ring. The decal still paints the ground round it and under it. On by
  default, on every kind of part; terrain always receives.
- **`--net-log-corrections`**: a client says, for every correction of its
  predicted character, how far and which way it was off, whether it was turned,
  the part it pushed and the predicted attributes that differ. The multiplayer
  guide has what each means and what a game does about it.
- **A swarm replicates itself** (ADR 0162): `Swarm.Replicates` and
  `ReplicationRadius`. Each replica has the swarm with the same agent numbers,
  read-only; `SetAgentTag` and `GetAgentTag` carry a number of the game's for
  each agent; `AgentAdded`, `AgentRemoved` (with the last tag, the place and
  an `Enum.SwarmAgentRemoval`) and `AgentTagChanged` fire on every machine;
  `SetAgentBody` gives an agent a part of that machine's own to move.
  `Player.ReplicationFocus` names the part a player is sent the world round.
  `NetworkService:GetStats()` gains `BytesSent`, `BytesReceived` and
  `SwarmBytes`. Fifteen hundred agents to three replicas: about 28 KB a second
  each of positions at 150 ms and 2% loss.
- **`BasePart.CastShadow`**: whether a part casts a shadow, into the sun's and
  every lamp's -- on by default, off for what is light or an effect. It
  replicates, a streamed world's records keep it, and it is under Appearance
  in the editor.
- **Remote messages carry buffers**: a `buffer` is a value a `RemoteEvent`, an
  `UnreliableRemoteEvent` and a `RemoteFunction` take, byte for byte.
- **`UnreliableRemoteEvent`** (ADR 0161): a `RemoteEvent` for what a game sends
  many times a second and replaces each time. A message is sent once and never
  again, so a lost one holds nothing else up; one that arrives after a later
  one is dropped; nothing is kept for an event the other side has not got or
  is not listening for; at most 16 KiB, refused at the call past it
  (`net.err.unreliable_too_large`). `NetworkService:GetStats()` gains
  `UnreliableSent`, `UnreliableReceived` and `UnreliableDropped`.
- **Particles take a picture, land, and run on the GPU** (ADR 0160).
  `ParticleEmitter` gains:
  - `Texture`, and a flipbook over it (`FlipbookColumns`, `FlipbookRows`,
    `FlipbookFramerate`, `FlipbookMode` -- `Enum.ParticleFlipbookMode` Loop,
    OverLife, Random);
  - `Rotation`, `RotationSpread`, `RotationSpeed`, `RotationSpeedSpread`;
  - `ColorOverLife`, `SizeOverLife` and `TransparencyOverLife`, curves that
    multiply the start and end values;
  - `Collision` (`Enum.ParticleCollision` None, SceneDepth, Terrain, Both),
    `CollisionResponse` (`Enum.ParticleCollisionResponse` Bounce, Stick,
    Kill), `Bounce`, `Friction` and `CollisionRadius`;
  - `Simulation` (`Enum.ParticleSimulation` Cpu, Gpu): on the GPU an emitter
    carries up to 262 144 particles, born, moved and collided in a compute
    shader -- a hundred thousand bouncing on the ground in a little over a
    millisecond a frame. They are drawn in no order among themselves, collide
    with what is on the screen and with the ground round the camera, and run
    on the CPU on a machine with no compute shaders.

  Every default is what an emitter did before: no picture, no turn, no
  collision, on the CPU.
- **Decals lay over and glow** (ADR 0160): `Decal.BlendMode`
  (`Enum.DecalBlendMode` Multiply, Alpha, Additive) and `Decal.Emissive`. A
  decal could only darken; now it can be a painted marking that reads on dark
  ground, or a ring of light.
- **`NetworkService:GetLocalAddresses()` and `NetworkService.Port`** (G41):
  this machine's addresses on its networks, best first -- the private ones,
  the default route's first, never loopback, link-local or a virtual adapter
  -- and the port of the match it hosts or joined: what a host shows the
  friends on its Wi-Fi to type.
- **Where each frame's time goes** (H0): `--frame-stats` prints a tree of
  scoped timers -- the simulation's phases, physics' steps, animation, the
  scripts by phase, the network, the UI, the renderer's passes and the waits --
  with each scope's median, p95, worst frame, calls a frame, and what no scope
  inside it accounts for.
- **The slowest frames, each on its own** (H11): `--frame-stats` ends with the
  worst five frames, and any other over twice the median, each as its own tree
  of scopes; and the Luau collector's steps are a scope of their own,
  `scripts.gc`, under whatever was running when one came.
- **Scripts run in the predicted step** (G37, ADR 0157):
  `RunService:BindToPredictedStep` runs inside the simulation step for every
  player's character a machine steps, on the player's own tick, and a client
  runs it again for every tick it steps over after a correction.
  `BasePart:BindToPredictedTouch` does the same when a character lands on a
  part. Attributes they write on the character are predicted state the server
  corrects. A dash and a stamp's jump pad run at 165 ms without a rubber band.
- **SMAA, TAA and FSR 1** (ADR 0158): `Enum.AntiAliasingMode` gains `SMAA`
  -- the default from Medium up -- and `TAA`, Ultra's, which stops thin lines
  crawling in motion; `GraphicsService.Upscaling = FSR1` brings a reduced render
  scale up with AMD's FSR 1, and `Sharpness` sets RCAS after it and after TAA.
  Phones upscale with FSR 1 at every level. `anti_aliasing = "taa"`,
  `--anti-aliasing=smaa`, `--upscaling=fsr1`, `--sharpness=0.4`.
- **The slowest frames are play's** (H11): the frames of a scene load and the
  three seconds after it (`--frame-stats-warmup=SECONDS`) are left out of the
  slowest-frames list, and every stretch of the frame has a scope of its own.
- **Input written by code** (G38): `RunService:BindToIntent` writes a
  player's intents -- a turn from the mouse, a gesture, a bot -- as an
  `InputAction` would. Intents now cross by number, each action's name once a
  connection (protocol 37).
- **A crowd of one skinned mesh is drawn in one call a pass** (H2): every
  skinned run's palettes in one storage buffer, read by instance; five hundred
  animated enemies went from 1,800 draws to a few dozen. `--no-instancing`
  draws every object alone, for comparing.
- **Animation is posed as often as it is seen** (H3): a rig neither the camera
  nor a shadow reaches is not posed, and a small one is posed every second,
  fourth or eighth tick; its clips keep time, and a joint a script or a `Bone`
  asks about is posed when asked. `AnimationPlayer.CullingMode =
  AlwaysAnimate` for the hero. A world with nobody looking poses every tick.
- **Skinned meshes have levels of detail** (H4), as static ones do.
- **`Swarm`** (ADR 0156): a crowd on open ground that the engine steers --
  hundreds of agents walking at a target, pushed apart, climbing and standing
  on one another, on the terrain's height and round obstacles, thinking less
  often far away; scripts add, remove, push and query in bulk. Five hundred
  and fifty agents cost about a third of a millisecond a tick.
  `QueryRadius(centre, radius, into?, flat?)` answers from the swarm's grid,
  fills a table given to it, and measures along the ground when `flat`.
- **`ScrollFrame` scrolls as a phone's list does** (G40), with no code:
  - **a fling**: let go while moving and it glides on and slows; a press
    catches it and presses nothing;
  - **elastic ends** (`ElasticBehavior`): a drag past the end gives, less the
    further, and springs back, and a fling into the end bounces --
    `CanvasPosition` never leaves the canvas;
  - **the bar's thumb drags**, and the track beside it pages;
    `ScrollBarImageColor` and `ScrollBarImageTransparency` colour it (a middle
    grey by default, where it took the frame's `BackgroundColor`);
  - **nested frames**: the drag's first pixels choose the innermost frame that
    scrolls that way, and it keeps the axis -- a column on a sideways carousel
    scrolls down, the carousel across, and a sideways drag over a list on
    `UIPageLayout` pages turns the page;
  - **a selection moved by a gamepad or the keys scrolls into view**;
  - `ScrollingEnabled`, `ScrollingDirection` (a frame that only scrolls across
    takes the wheel across), `AutomaticCanvasSize` (the canvas grows to hold a
    list, a grid or its children, with its padding), and the read-only
    `AbsoluteCanvasSize` and `AbsoluteWindowSize`;
  - new example `examples/34-settings-list`, and `examples/32-menus` sizes its
    inventory's canvas with `AutomaticCanvasSize` instead of a `Heartbeat`.
- **Stamps 2.0** (ADR 0155), the prefab model at the level of the other
  engines':
  - a copy has **one pivot**: move it anywhere and its parts stay where the
    stamp puts them, so a part moved in the stamp moves in every copy (G1);
  - **stable ids** for a stamp's nodes: renaming a part keeps every copy's
    overrides of it;
  - **nested stamps** and **variants** (a stamp that is a copy of another, as
    deep as wanted), with **Apply to** the variant or its base;
  - a copy **adds** children and **disables** its stamp's, and enables them
    back;
  - **parameters**: a stamp declares values a copy shows first in Properties,
    each an attribute of the copy's root that drives the properties it is
    given; `Instance.stamp(name, linked, parameters)` sets them, refused and
    never clamped out of range; `Instance:GetStamp()`;
  - a **`Construct`** module that builds a stamp's parts from its parameters;
  - stamps in **`ContentProvider:PreloadAsync`**, and **`@engine/stamppool`**;
  - in the editor: revert a part or a whole copy, apply the whole copy to its
    stamp, make a variant, open the base, select every copy, replace a copy
    with another stamp, unused overrides kept and cleaned up, and thumbnails
    for variants and stamps that hold stamps.
- **A remote carries `Color3`, `CFrame`, `Vector2`, `UDim`, `UDim2` and
  `EnumItem`** (D495), each arriving equal to what was sent.
- **`Enum.ScaleType.Fit` and `Crop`**: an image shown whole in its box's
  shape, centred, or filling the box with the overhang cut.
- **Joints travel** (NA34): attachments, every constraint, welds, movers and
  `NoCollisionConstraint` reach the replicas, and the machine that owns a
  vehicle's parts solves its hinges and drives its motors; `Workspace.Gravity`
  travels too.
- **A worse network, on purpose**: `engine-host --net-delay=MS --net-jitter=MS
  --net-loss=PCT` puts a link conditioner below the transport of a client's
  link; refused by a shipping build. And `netcode_acceptance`, a gate that
  walks a predicted character between a real server and a real client at 0 to
  300 ms round trip with loss, jitter and long frames, and prints the table.
- **`NetworkService.HostFailed`**, and `GetStats()`'s `Corrections`,
  `InterpolationDelay` and `InputReanchors`.
- **`--saves=DIR`**; a dedicated server keeps `saves-server` beside the
  player's saves.
- `examples/33-options`: the engine's options screen and a language button.

- **`@engine/settings`** (ADR 0147): an options screen in one call.
  `settings.open()` puts a Graphics page and a Display page over the game, a
  row for each setting this build draws by, with Apply, Revert, Defaults and
  Back; a mouse, a finger, a gamepad and the arrow keys drive it. It takes a
  theme and a list of settings to hide, and its words are catalog keys a
  game's own catalog can replace.
- **Project Settings has Graphics and Display** (ADR 0147): `project.toml`'s
  `[graphics]` and `[display]` as a form in the editor. A ticked setting is
  the project's; an unticked one shows what the level gives and is not in the
  file.
- **`LocalizationService`** (ADR 0154): a game's text by key, in the player's
  language. `Translate(key, arguments)` with `{name}` placeholders, `Locale`,
  `GetLocales()` and `LocaleChanged`. A project's catalogs are
  `i18n/<locale>.json`, read again when one changes while a game is being
  made and carried into an export; `[project] default_locale` names the one
  a key falls back to. The player's choice is kept in their folder. The
  engine's own text is read by the same call. `ludwerk check` fails when a
  locale lacks a key the default locale has.

- **`GraphicsService`** (ADR 0147): a script reads and writes the machine's
  graphics and display settings -- `QualityLevel`, the shadow, post and
  detail settings, `WindowMode`, `Resolution`, `Monitor`, `VSync`,
  `MaxFrameRate`, `BackgroundFrameRate` -- with `ApplyPreset`,
  `ResetToDefaults`, `GetSource`, `IsApplied`, `GetGroupLevel` and
  `SetGroupLevel`, `GetMonitors`, `GetSupportedResolutions`, `GetRefreshRate`
  and `QualityChanged`. `SaveAsync` keeps the player's choices in
  `settings.json` in their folder, and a game starts in them. Eight enums with
  it. In `project.toml` any setting is a key, as its name in snake case, and
  `[display]` gains `window_mode`, `resolution`, `monitor` and
  `remember_player_settings`.

- **UI that lays out in grids and pages, fits any screen, and is driven by a
  gamepad** (ADR 0128). `UIGridLayout`; `UIPageLayout` with `Next`, `Previous`,
  `JumpTo`, `JumpToIndex`, `CurrentPage`, `PageEnter`, `PageLeave` and
  `Stopped`, turned by a swipe, the wheel and a gamepad's shoulder buttons;
  flex in `UIListLayout` (`HorizontalFlex`, `VerticalFlex`,
  `ItemLineAlignment`, `AbsoluteContentSize`) and `UIFlexItem`; `UIScale`,
  `UIAspectRatioConstraint`, `UISizeConstraint` and `UITextSizeConstraint`;
  `CanvasGroup`, a frame drawn as one picture with `GroupTransparency` and
  `GroupColor`; `UIDragDetector`; and selection -- `UIObject.Selectable`,
  `NextSelectionUp`/`Down`/`Left`/`Right`, `SelectionImageObject`,
  `SelectionGained`, `SelectionLost`, and `UIService.SelectedObject`,
  `AutoSelect` and `SelectionChanged`, moved by the d-pad, the left stick and
  the arrows and pressed by `ButtonA` or Enter. Eight enums with them.
  `examples/32-menus`.

- **`Highlight`, `Beam` and `Trail`** (ADR 0129): an outline and a tint over a
  part or a model, through walls or not, in the colours it is given; a band
  between two attachments, straight or curved, with a texture that can run;
  and the ribbon two attachments leave behind. Pictures, drawn on the frame.
  `[render] max_highlights` is the budget, 32 by default.
- **Nine sound effects** (ADR 0131): `ReverbSoundEffect`, `EchoSoundEffect`,
  `EqualizerSoundEffect`, `LowPassSoundEffect`, `HighPassSoundEffect`,
  `DistortionSoundEffect`, `CompressorSoundEffect`, `ChorusSoundEffect` and
  `PitchShiftSoundEffect`, under a `Sound` or an `AudioGroup`, in the order of
  their `Priority`.
- **`HapticService`** (ADR 0131): `SetMotor`, `Vibrate`, `IsVibrationSupported`,
  `IsMotorSupported`, and `Enum.VibrationMotor`.
- **A material is what a thing is made of** (ADR 0117): `Friction`,
  `Restitution`, `FootstepSound` and `Tags` on a material. A part collides
  with what it wears and a terrain with what each piece of ground is drawn as;
  `RaycastResult.Material` says what a ray met and
  `CharacterBody.FloorMaterial` what a character stands on. The engine's
  ground materials are loadable (`Material.load("engine://terrain/ice")`) and
  a part may wear one; ice and snow slide, mud grips.
- **`Constraint:GetMotorForce()` and `GetMotorTorque()`**: what a joint's or a
  mover's own motor used over the last tick, apart from what the joint bore.
- **Movers and powered joints** (ADR 0127): a part turns, moves and is held in
  place by instances, with no script running each tick. `LinearVelocity`,
  `AngularVelocity`, `AlignPosition`, `AlignOrientation`, `VectorForce`,
  `Torque`; `HingeConstraint.ActuatorType` with a motor and a servo;
  `PrismaticConstraint` (a rail), `RopeConstraint` (with a winch),
  `RodConstraint`, `SpringConstraint`, `NoCollisionConstraint`. `Visible` draws
  any of them in the editor and under the overlay, and a rope, a rod and a
  spring in the game.
- **A joint does work and gives under load** (ADR 0127's amendments): a
  `BallSocketConstraint` holds a pose (`ActuatorType = Servo`,
  `TargetOrientation`); aligns and servos take `Stiffness` and `Damping`; an
  align pulls back on what it follows (`ReactionForceEnabled`,
  `ReactionTorqueEnabled`); every constraint has `BreakForce`, `BreakTorque`,
  `Broken`, `GetForce()` and `GetTorque()`.
- **A run starts in the scene it is told** (D468): `--scene=PATH` and
  `--scene-data=JSON` on `engine-host` and on `ludwerk dev`; the JSON is what
  the scene's `GetLoadData` answers.
- **A character is pushed, floats, swims and flies** (D466): `ApplyImpulse` on
  a `CharacterBody` does what it says and its `LinearVelocity` can be written;
  `GravityScale`, `SwimSpeed`, `FlySpeed`, `Flying`; `Enum.CharacterState` gains
  `Swimming` -- entered by itself in a `Water` and in a fluid block -- and
  `Flying`. `@engine/camera.firstPerson`.
- **`BasePart.Mass` and `AssemblyMass`** (read-only), **`LinearDamping` and
  `AngularDamping`**, **`BasePart.Collided`** -- where, which way and how fast
  two parts met, for a part whose `ContactDetails` is on -- and
  **`Workspace:GetBodiesInSphere`**.

- **Gestures** (D462): `InputService.TouchSwiped`, `TouchTapped`,
  `TouchLongPressed`, `TouchPinched`, `TouchPanned`, `SwipeThreshold`, and
  `Enum.KeyCode.SwipeUp`, `SwipeDown`, `SwipeLeft`, `SwipeRight` for an action
  to bind.
- **`UIObject.Active`** (D452) and **`ImageLabel.ImageTransparency`** (D453).
- **`InputService.TouchAvailable`, `KeyboardAvailable`, `GamepadAvailable`**
  and **`RunService.Platform`** with `Enum.Platform` (D456).
- **`Lighting.ExposureMin` and `ExposureMax`** (D460).
- **A `RemoteEvent` keeps what arrives before anybody listens** (D451), and a
  client that joins late has every attribute as it stands (D457).
- **An installation checks a project's types with nothing else installed**
  (ADR 0152, D455), and `ludwerk new` makes an id a store accepts (D454).

- **`CryptoService`** (ADR 0151, D442): `RandomBytes`, `RandomInteger` and
  `UniqueId` from the operating system's generator; `Sha256`, `HmacSha256` and
  `SecureEquals`; `HashPasswordAsync` and `VerifyPasswordAsync` (Argon2id, off
  the tick). libsodium 1.0.22 is vendored for it.
- **`Camera:WorldToViewportPoint`, `ViewportPointToRay` and
  `ViewportPointToWorld2D`** (D436), in window pixels.
- **`ScreenGui.ReferenceHeight`** (D437): a tree laid out for a window that
  tall and scaled to the one it has. **`UIService.ViewportSize`**.
- **`Player.Character2D`** (D434): a player's character on the 2D plane, the
  same one slot as `Character`, typed `Part2D?`. Its machine is sent what is
  near it and does not draw it from the past.
- **`BillboardGui.Adornee` takes a `Part2D`** (D435), and a billboard may be a
  `Part2D`'s child.
- **`Enum.KeyCode.Virtual5` to `Virtual16`** (D443).
- **A touch says when the interface took it** (D444): the `uiConsumed`
  argument of `InputBegan`, `InputChanged` and `InputEnded` is true for a
  finger that came down on the interface.

- **A plain part can glow**: the engine default material declares `Emissive`
  beside `Color` and `Transparency`, so `part:SetMaterialParameter("Emissive",
  ...)` works with no material authored -- an ember, a lamp, a sign for bloom
  to act on.
- **`Instance:GetFullName()`**: the path to an instance, `Workspace.Trees.Oak.Trunk`.
- **A new project checks and formats as the engine does**: the starter carries
  a `.luaurc` with the `@engine` and `@std` aliases, so `ludwerk check`
  resolves `require("@engine/camera")`, and a `stylua.toml`, so `ludwerk fmt`
  writes four spaces.
- **A phone starts a level lower and renders the world under a cap** (ADR
  0147, the mobile ledger): a project that names no level is `medium` on
  Android, and each level caps the world's shorter side there (720, 900, 1080,
  none) while the interface stays at the display's resolution. `[graphics]
  render_cap` sets the cap by hand on any platform.
- **A platform's own graphics table**: `[graphics.android]`,
  `[graphics.windows]`, `[graphics.linux]` and `[graphics.macos]` are read
  over `[graphics]` on that platform.
- **`--frame-report=SECONDS`** and `[debug] frame_report_seconds`: a line in
  the log every so many seconds with the frame rate, the median, p95 and worst
  frame, the draws, the triangles and the resolution the world was rendered at.
- **A water's bed, cut into the ground** (ADR 0146): `Water:Carve()` lowers
  the terrain under a river, a lake or a pool to its depth, with a bank
  `Water.BankWidth` wide sloping up to its edge, and answers how many columns
  it lowered; a river's bed follows the river downhill. In the editor the
  Water panel's **Carve bed** does it for the water in hand, as one undo step.
- **Rivers that descend along a curve, and lakes of any outline** (ADR 0146):
  `Enum.WaterShape` gains `River` -- a ribbon along the smooth curve through
  its `WaterPoint` children, each point's height the surface's there --
  `Lake` -- the inside of the closed curve through them, level at
  `SurfaceLevel` -- and `Pool`, the new name of `Box`. `WaterPoint` gains
  `Width`, `Depth` and `Sharp`. A river runs faster where it drops, and what
  floats in it is held up at its height there; `Water:GetHeightAt` answers
  that height. `Box` and `Spline` are as they were. In the editor the Water
  tool draws all of them: River, Lake (click round a shore), Pool, Ocean.
- **The Water tool** (ADR 0146, its first stage): **Tools > Water** in the
  editor. A river is drawn a click a point, with the next stretch shown
  before the click; a lake is dragged as a rectangle, level at the height the
  drag began at; a sea is a click at the height it comes to. Points and
  corners are handles to drag, Delete removes a selected point, and every
  click and drag is one undo step. The panel edits the height, the width, the
  depth and the flow of the water in hand.
- **A world larger than memory** (ADR 0149). Ground a game changes or makes
  is written to a cache on disk when the camera leaves and there is more of
  it than fits, and streams back from it, so a script that writes a world a
  tile a frame holds the ground round the camera and a budget of the rest; a
  terrain under 256 cells, the ground under a body that can fall, a match and
  the editor's own brush keep theirs in memory.
  **`ludwerk terrain import <source> [path] --size=<metres>`** lays a
  project's terrain from a heightmap, a folder of tiles named
  `<anything>_x<column>_y<row>`, a `.luau` file that returns `function(x, z)`,
  or `hills`, and saves the scene. The editor's Create tab lays the same
  sources at any width up to 32 768 columns -- past 4 096 a tile at a time,
  with a progress bar and Cancel -- and gains **From a function**.
- **A world sixteen kilometres across is flown at a frame rate** (ADR 0150).
  The far ground of a streamed terrain is kept on disk as it is drawn -- a
  file a node, beside the project under `.engine/terrain-pyramid/`, made by
  `ludwerk terrain import` and by the build, and in the background for a
  project saved before -- so ground past the load radius is read, not worked
  out from every cell under it. On a 16 km world of 65 536 cells: a flight
  corner to corner at 200 m/s is 12 ms at the 99th percentile with no frame
  over 33 ms, where it was 33 ms with 77 over; memory is level at some
  800 MiB where it reached 2.6 GiB; and a 10 km teleport has ground under the
  player in a quarter of a second. Nothing in the API changes.
- **The terrain's Foliage tab lists the chosen layer's meshes**, each with its
  mesh, share, sizes and wind, and a button to remove it -- no trip to the
  Explorer to change what a layer grows.
- **A terrain layer's repeat is broken up** (ADR 0113's amendment): three
  material fields, read by a terrain -- `TilingVariation` (a colour drift
  over tens of metres, 0.5 by default), `TilingFarScale` (a second, larger
  sample of the textures blended in with distance, 6 by default) and
  `HexTiling` (the textures on random hexagonal cells, off by default). A
  field of grass no longer reads as a grid from a distance; set them to 0, 1
  and off for the plain repeat.
- **The terrain editor asks before Clear All Ground, Replace with Flat Ground
  and Replace with Hills**; gives a terrain the eight starter materials in one
  action (**Use the Starter Materials**); and speaks through the engine
  catalog, every word of it an i18n key (R3 reaches the editor, 2026-09-29).
  The Settings tab is Setup everywhere, and its voxel size reaches 64 m.

- **`engine-host --pace=HZ`**: a headless run waits out each frame's share of a
  second, the wait left out of `--frame-stats`, so work done beside the frame
  is measured against a camera moving as fast as a player's.

- **`[graphics] contact_shadows`** and `--[no-]contact-shadows`, so the sun's
  contact shadows can be turned off alone (terrain audit T0).
- **Test instruments on `engine-host`** (terrain audit T0):
  `--screenshot-every=N` photographs a run every N frames;
  `--debug-view=holes|level|sky|shadow|occlusion` draws what the terrain is
  made of in place of the picture; `--terrain-detail=full` draws every terrain
  at its finest level. `imgholes` counts sky seen through or instead of the
  ground in such a picture.
- **Two more terrain debug views** (terrain audit T3): `--debug-view=shadow`
  draws blue where the ground faces the sun, and `imgshadow` counts faces the
  shadow map or the contact mask darkens; `--debug-view=bend` draws what the
  shading does to the mesh's normal, and `imgsteps` counts where it jumps;
  `--debug-view=albedo` draws the colour the ground is lit as, and
  `--debug-view=material` the layer each pixel is drawn as.
- **A `TextInput` edits as every text field does** (ADR 0139): selection by
  keys and by the mouse (drag, Shift, double and triple press), words, the
  line's and the text's ends, Ctrl+A/C/X/V through the system clipboard, undo
  and redo, several lines, a caret that blinks and a field that scrolls to keep
  it in view, an input method's composition drawn at the caret, and Tab to the
  next field. New properties: `PlaceholderColor`, `MultiLine`, `Editable`,
  `MaxLength`, `Masked`, `MaskCharacter`, `ClearTextOnFocus`,
  `SelectAllOnFocus`, `ReleaseFocusOnSubmit`, `RevertOnEscape`, `KeyboardType`,
  `CursorPosition` and `SelectionStart`. New events: `Submitted`,
  `TextChanged` and `InputRejected`; `FocusLost` gains its reason, in the new
  `Enum.FocusLossReason`. New methods: `CaptureFocus`, `ReleaseFocus` and
  `IsFocused`. `UIService:GetFocusedTextInput()`, with
  `UIService.TextInputFocused` and `TextInputFocusReleased`. A phone raises
  the keyboard `Enum.TextInputKeyboard` names.
- **`Script.RunContext`** (ADR 0138): `Server`, `Client` or `Shared` (new in
  `Enum.RunContext`, and the default), deciding where a script outside the
  script services runs. Inside one, the service decides, and moving a script
  in writes its side. A solo or hosting machine runs each script once. The
  Explorer marks the side, the *Insert* menu has a script for each, and in a
  project with multiplayer the script editor and `ludwerk check` warn about a
  script reaching for the other side's things.
- **Each package carries only its own side's code per script** (ADR 0138 §5):
  a stamp can hold a server half and a client half.
- **A joined client runs its own package's client and shared scripts** under
  what the server sends -- a scene's parts, a stamp placed at run time and its
  clones (ADR 0138 §6). **Protocol 29**: `Spawn` carries where an instance was
  authored.

- **Scripts that run at the display's rate** (ADR 0136).
  `RunService:BindToRenderStep(name, priority, fn)` and `UnbindFromRenderStep`,
  with `Enum.RenderPriority`, run every drawn frame by priority before
  `PreRender`; `BasePart`, `Attachment` and `Camera:GetRenderCFrame()` answer
  where a thing is drawn this frame there, and its `CFrame` anywhere else. A
  camera written in a render phase is *presented* -- drawn exactly as written,
  and the simulation's camera from the next tick -- so the simulation and the
  world hash never read a value written between two ticks. `@engine/camera`'s
  rigs turn and follow every frame, reading their `TurnAction` and
  `LookAction` from a render-rate context, and move once a tick where nothing
  draws. No determinism trace moved.
- **Water** (ADR 0118, the game-ready plan's B8). A `Water` is a sea
  (`Ocean`), a lake (`Box`) or a river (`Spline`, along its `WaterPoint`
  children, flowing at `FlowSpeed`); its `WaterWave` children, up to eight, are
  its waves. One function of them and `RunService.SimTime`, in the engine's own
  maths and in the shipped `water` surface shader, so what floats rides the
  surface that is drawn: a simulated part below the water's `Density` floats,
  held up at 27 points through its volume and dragged towards the current, the
  flow and the waves' own motion -- a hull pitches, rolls and rights itself.
  `BasePart.Buoyant` opts a part out; a part a weld drives is carried, not
  floated. `Water:GetHeightAt` and `GetNormalAt` for scripts, and
  `BasePart:ApplyImpulseAtPosition` and `ApplyAngularImpulse` for thrust and
  steering. Protocol 28. Every determinism trace was re-recorded: `Buoyant` is
  a new property in the world hash; a `water` scenario joins them.
  `examples/11-ocean` is rewritten on it -- a boat that is a body, no wave
  function of its own -- and `examples/31-lake-and-river` carries logs down a
  river into a lake.
- **`DragDetector`** (ADR 0126 F1b). A part or a model dragged with the pointer
  or a finger, with no input code: along a line, across a plane or the view,
  turned about an axis or as a trackball, or left to a script, within limits
  measured from where it rested; `DragStart`,
  `DragContinue` and `DragEnd` with the player. The drag travels as rays
  (protocol 26) and the authority moves the part; a physical drag hands the
  part to the player while it is pulled. `examples/30-interactions` has a
  drawer and a lever.
- **A voxel holds two materials and a blend** (ADR 0114, B3).
  `Terrain:PaintBall` takes `{ Mode, Strength, Falloff }`: `Replace`, `Blend`
  over what is there, `Under` it, or `Erase` the paint; the seam between two
  materials is a soft height blend. `.lterrain` version 4 and protocol 27 carry
  the paint; an unpainted terrain is the bytes it was.
- **Terrain tools** (B4): the editor's eyedropper (`Alt`+click), paint masks by
  slope, height and material, *Replace Material*, a noise hill generator, and
  heightmap export as 16-bit PNG or RAW.
- **The ground replicates** (ADR 0135). Terrain and block edits travel to every
  player as whole chunks, and a player joining late receives the ground as it
  is. `Terrain:GrowBall` raises and lowers the ground as the editor's brushes
  do.

- **A scene closes as a game does, and talks to the game by message** (ADR
  0124). The global `scene` is the scene open now; `scene:BindToClose` runs
  when it closes and a `LoadScene` waits for it (`[scene]
  close_grace_seconds`, 5 by default), so a level saves before the next opens.
  `game` and `scene` are mailboxes: `SendMessage(topic, ...)` and
  `BindToMessage(topic, fn)`, deferred, the values copied, and a binding goes
  with its script's scene.
- **`ClickDetector` and `ProximityPrompt`** (ADR 0126). A part is clicked, or a
  prompt near it triggered with a key, a gamepad button or a tap, with no input
  code: the engine casts the pointer once a tick, measures reach from the
  character, draws the prompt with its hold, and fires `MouseClick(player)` or
  `Triggered(player)`. In a match the client fires at once and the server fires
  after checking the player could reach it (protocol 21). The editor draws the
  reach of a selected one; `examples/30-interactions` is a room with a button
  and a door.
- **`ContentProvider:PreloadAsync`** (ADR 0131). Loads a list of content names
  and instances -- every asset an instance names -- before anything shows them,
  and yields until each has arrived; a callback reports each item's
  `Enum.AssetFetchStatus`. `ContentProvider.RequestQueueSize` counts what is
  still on its way. Every determinism trace was re-recorded: a new service is a
  new instance in the world hash.
- **A scene prepared in the background** (ADR 0125).
  `SceneService:LoadSceneAsync(path, { Activate = false })` returns a
  `SceneLoad` at once: the file is read and parsed off the main thread and its
  meshes loaded while the current scene plays, `Progress` fills a bar, `Ready`
  fires, and `Activate()` switches. `examples/24-scenes` prepares its arena so.
- **Foliage** (ADR 0116, the game-ready plan's B6). A `FoliageLayer` under a
  `Terrain` says where things grow -- the materials it grows on, slope, height,
  clumping, spacing, draw distance and seed, and never under a roof -- and its
  `FoliageMesh` children say what, in what share, how large and how much the
  wind moves them. No instance per blade: each tile of ground is grown on the
  job threads as the camera nears it, from the same rules and seed on every
  machine, and a compute pass on the GPU culls the field into indirect draws,
  picking a level of detail and fading it out at its distance. Visual only:
  not saved, replicated or hashed. The Terrain panel has a Foliage section and
  a brush that paints a layer's density by hand; `[render] foliage_density`
  and `foliage_shadow_distance` scale it (lower on Android). A field to the
  horizon costs 1.5 ms at 1080p (`tests/perf/foliage`); `examples/29-meadow`
  shows it, and the flagship's island grows grass. The RHI gained compute
  pipelines, storage buffers and indirect draws for it.
- **Wind** (ADR 0115, the game-ready plan's B5). `Workspace.GlobalWind`,
  `WindGusts` and `WindTurbulence`, and `Workspace:GetWindAt(position)`: one
  function of them and the simulation clock, the same in C++ and HLSL, so a
  script reads the wind the picture shows. Visual only -- it pushes no body and
  stays out of the world hash. Particles drift with it where
  `ParticleEmitter.WindAffectsDrift` is on; surface shaders read
  `SurfaceInputs.Wind` (contract 1.1; the block header is 64 bytes). The wind
  replicates as `Workspace`'s properties (protocol 19). A still world, the
  default, looks as it did.
- **Rules paint a terrain by slope and height** (ADR 0113 §2, the game-ready
  plan's B2). `Terrain:GetRules()` / `SetRules()`: each rule draws a layer
  where the ground is between two slopes and two heights, over the layers it
  names, with an edge as wide and ragged as it says, in order. A new terrain's
  one rule is the automatic rock it always had -- now something a project can
  turn off, retune or follow with a snow line. Rules are drawn, not written;
  `ApplyRules` and the editor's **Apply to Voxels** write them. The same
  evaluation runs on the CPU with the shader's noise, so what the game reads
  under a slope is what is drawn there. The editor's Terrain panel has a
  **Rules** section, live.
- **A terrain's layers are materials, with textures** (ADR 0113, the
  game-ready plan's B1). A voxel's material id is the terrain's layer of that
  number, a material asset; a new terrain's eight are the engine's own --
  grass, sand, rock, snow, mud, sandstone, basalt, ice -- built in, with
  colour, normal, surface and height textures drawn from noise, so id 3 is
  still rock and an old world opens as it was, now textured. `GetLayers` and
  `SetLayers` change the list (saved with the scene; the editor's Paint
  section replaces, adds and removes); layers are sampled triplanar at their
  material's `TileSize`, and a pixel blends the layers of its triangle's
  corners. Materials gain `HeightMap`, `Triplanar` and `BlendSharpness`.
  `RaiseBall` takes a material, and `WriteHeights` one per column. Each
  terrain now draws its own layers, where every terrain used to share the
  first one's palette.
- **An exported game carries bytecode, not source** (ADR 0112, the
  game-ready plan's A3). `ludwerk build` compiles every script in `game/src/`
  with the engine's own compiler -- `init.luau` ships as `init.luauc` -- and
  an error in a player's log still names the script and the line. Bytecode
  from another engine build is refused by a keyed error naming both versions.
  `[export] ship_source = true` (a **Ship source** box in the Export window)
  keeps the source. A build without a compiler now runs a compiled game.
- **An FBX or Collada file with bones brings its skeleton, skin and takes**,
  where it used to import as a static mesh. An `AnimationPlayer` under the
  `MeshPart` plays a take by the name after its `|`. The armature's scale (the
  centimetre conversion) is folded into the bones, the nodes above the
  skeleton cost no palette slots, and a rig past sixty-four bones keeps the
  ones that move a vertex. Projects recompile their meshes on the next open.
- **`SaveService`: a game keeps what outlives a run** (ADR 0111, the
  game-ready plan's A2). `GetSlotAsync(name)` hands back a `SaveSlot` --
  `Get`, `Set`, `Update`, `Remove`, `GetKeys`, `SaveAsync`, `Changed`,
  `Recovered` -- holding strings, numbers, booleans, the geometry and colour
  types, sequences and tables of them, each read back as the type it went in.
  What is set is written by itself, at most once a second, on close and when a
  phone backgrounds the game; a damaged file is read from its backup. Saves go
  to the player's folder named by `[project]` company and name, and to the
  project's `.engine/saves/` from the editor and `ludwerk dev`. `Version` and
  `OnMigrate` bring an older slot up to date. `[save] max_slot_bytes` and
  `max_slots` bound it. The editor has a Saves panel (View > Saves) and the
  CLI `ludwerk saves list|clear`.
- **A mesh dragged from the Content browser becomes a `MeshPart`**: onto the
  viewport it stands where it was dropped, onto a row of the Explorer it goes
  under it, and either way it measures what the mesh measures. A mesh given to
  a `MeshPart` in Properties sizes the part the same way.
- **`https://` in `net.request`, from the platform** (ADR 0063, the game-ready
  plan's A1): WinHTTP on Windows, the system's OpenSSL on Linux (loaded at the
  first request), `NSURLSession` on macOS and the Java stack on Android. The
  certificate is always checked against the device's trust store, with no way
  to turn that off. Redirects are followed for every scheme: at most five,
  never from `https` to `http`, a `303` as a `GET`, and no `Authorization` or
  `Cookie` to another host.
- **Cameras on screens** (ADR 0107, V0 and V1). A `CameraTexture` draws a
  `Camera` into `view://<ViewName>`, and anything that takes a texture shows
  it: an `ImageLabel` on a `SurfaceGui` or a `ScreenGui`, a `Decal`, a
  material's map.
  - Feeds are drawn before the main view, each with its own exposure, shadow
    fit and targets.
  - A budget (`[render] max_views_per_frame`, 4) draws the oldest picture
    first; `UpdateInterval` and `Resolution` set what each costs.
  - `Quality = Simple` leaves out shadows and effects.
  - F3 and the editor's Stats list every view and its cost.
  - A camera texture does not replicate, because a camera does not.
  - `examples/26-security-cameras`: a security office at night, six feeds on a
    monitor wall and one on a tablet, and three items turning in inventory
    slots.
- **`SubWorld`: a scene running beside this one** (ADR 0107, V4). `Load()`
  starts a scene in a world of its own -- its instances, scripts, physics and
  clock -- and draws its camera into a `view://` name.
  - The two talk only through `Send`/`Received` and, inside,
    `SceneService:SendToHost`/`HostMessageReceived`: plain values, delivered at the
    other side's next tick.
  - Its input is only what `SetInputState(action, value)` gives it.
  - It ticks once per tick of the world running it, and its world hash is its
    own. It runs its scene's own code, never the project's global code.
  - At most `[render] max_sub_worlds` (2) run at once, one level deep. It does
    not replicate.
  - `examples/28-arcade`: two playable cabinets.
- **Mirrors and portals** (ADR 0107, V3). `@engine/views` has
  `views.mirror(part)` and `views.portal(a, b)`. Each is a `CameraTexture`
  whose camera is moved every tick, with its picture on the part's front face.
  - The pictures are exact from any angle: the camera looks straight through
    the glass, and the glass is cut out of its picture with the new
    `ImageLabel.ImageRectOffset` and `ImageRectSize` (a negative size reads
    backwards).
  - `Camera.ClipPlane` and `ClipPlaneEnabled` give a camera an oblique near
    plane, so what stands behind the glass is not in the picture.
  - The RHI gains `RasterizerState::depthClip`, on for the world's geometry,
    which the oblique plane needs.
  - `examples/27-mirrors-and-portals`.
- **Instances in the UI** (ADR 0107, V2). A `ViewportFrame` draws the parts and
  models inside it, unsimulated, by its own light (`Ambient`, `LightColor`,
  `LightDirection`), with no sky, so with `BackgroundTransparency = 1` only
  they show.
  - With no `CurrentCamera` it frames its contents itself.
  - It is redrawn only when something inside it, its camera, its light or its
    size changes.
- **The editor, remade after VS Code** (the owner's queue of 2026-09-27, Q2):
  - Dark Modern and Light Modern colours and metrics.
  - A command palette (`Ctrl+Shift+P`, `F1`) holding every menu and toolbar
    action by name, plus "Insert: <class>" and "Color Theme" commands.
  - Quick open (`Ctrl+P`) for scenes, stamps, materials, shaders and
    scripts.
  - A status bar: run state, the open scene and whether it is saved, error
    and warning counts, the selection and the tool. It turns blue while the
    game runs.
  - An activity bar that switches the side bar between Explorer, Content,
    Run and Debug and the world tools, and folds it away (`Ctrl+B`).
  - VS Code's menus (File, Edit, Selection, View, Go, Run, Help), drawn from
    the palette's commands, and one toolbar row in place of the ribbon's
    four tabs.
  - The match's players and dedicated server at the top of Run and Debug
    and in the Run menu.
  - A Console filtered by level and text, with its prompt labelled.
  - Notifications as cards in the bottom-right corner.
  - "Insert Object" first on the Explorer's right-click menu.
  - Close buttons only on the active or hovered tab.
  - The keys VS Code users expect: `F5`, `Shift+F5`, `Ctrl+J`, `` Ctrl+` ``,
    `Ctrl+,`, `Ctrl+Shift+E`/`A`/`D`.

  - A Welcome page (Help > Welcome).
  - Search on the script colours and shortcuts pages of Preferences, and
    Preferences: Keyboard Shortcuts in the palette.
  - One heading style in Properties.

  A layout saved before this is rebuilt once, to the new arrangement.
  `--editor-drive=FILE` drives the editor from a script of input events, so
  every state of it can be photographed with nobody at the machine.

- **`UIGradient` and `UIStroke`** (ADR 0110). A gradient colours and fades the
  element it is under -- background, picture and text -- along a
  `ColorSequence` and a `NumberSequence`: **linear, radial or conical**, rotated
  and offset, with a `Scale` and **Clamp, Repeat or Mirror** tiling. A stroke
  outlines an element's **border** -- **outer, centred or inner**, with an
  offset, round, bevel or miter corners, and several per element ordered by
  `ZIndex` -- or a text object's **text**, hollow lettering included; its
  thickness is pixels or a fraction of the element (of the font size, on text),
  and a `UIGradient` under it colours it. Rich text takes
  `<stroke color thickness transparency joins sizing>`. Both work in a
  `SurfaceGui` and a `BillboardGui`. `examples/25-gradients-and-strokes` shows
  every one; `docs/manual/ui/gradients-and-strokes.md` explains them.
- **`ColorSequence`, `NumberSequence` and their keypoints** (ADR 0110): two to
  twenty stops from time 0 to time 1, immutable, compared by value. Properties
  and attributes hold them, a scene saves them, and attributes replicate them
  (protocol 18). A sequence in the Properties panel is a bar with a handle per
  stop.
- **`imgprobe`** beside `imgcmp`: asserts the colour at named points of a
  screenshot, which is how `ui_appearance_gate` checks eighteen claims about
  gradients and strokes without a golden image.
- **A game's identity, set once** (ADR 0104): `[project] company`, a validated
  `[project] version` (`X.Y.Z`), `[window] fullscreen` and `resizable`, and
  `[window] width` / `height` read as `size`. A PNG `[project] icon` is what the
  window wears while the game is being made. `assetc icon <png> --out <dir>`
  makes every platform's icons from that one picture: a multi-size `.ico`, a
  512-pixel PNG, and Android's launcher and adaptive icons.
- **A packaged game opens no console window** on Windows: the `player` and
  `shipping` hosts are Windows-subsystem programs, and one started from a
  terminal still prints there. `luaug build` leaves the editor's own files in
  `.luaug/` out of the game.
- **`luaug build` exports for Windows, Linux and Android** (ADR 0104):
  `--target=windows|linux|android` (`win64` is still `windows`), from any
  desktop that has the target's player. Windows: the folder, the game's icon and
  a VERSIONINFO (product, company, version) stamped into the executable, and a
  `.zip` beside it. Linux: the folder with a `.desktop` entry and a 512 px icon,
  and a `.tar.gz` whose headers carry the execute bit wherever it was made.
  Android: `dist/android/<name>-<version>.apk` from the prebuilt `libmain.so`
  and the shipped Gradle template -- no NDK -- with the package, version code,
  orientation and launcher icons from `[export.android]`, signed with a debug
  key made once per machine and checked by `apksigner`.
  `--progress=json` writes one line per step for the editor.
- **A dedicated game exports as two packages** (ADR 0105): `[export]
  multiplayer = "dedicated"` makes `windows`/`linux`/`android` its clients and
  `windows-server`/`linux-server` its server, and neither carries the other's
  scripts, scene services, storage or `global.json` folder -- pack and
  partition cache included. A server package starts serving with no arguments:
  its `luaug.toml` says `[network] role = "server"`.
- `assetc archive` writes a `.zip` or `.tar.gz` of a folder; `iconpatch
  version` stamps a VERSIONINFO and `iconpatch info` prints one.
- **Android release signing** (ADR 0104 §3): `[export.android] release = true`,
  `keystore` and `key_alias` sign the APK with the project's own key, its
  passwords from `LUAUG_ANDROID_KEYSTORE_PASSWORD` / `LUAUG_ANDROID_KEY_PASSWORD`
  or the per-user store -- never from `luaug.toml`. `luaug keystore new --alias
  --out [--name] [--validity] [--remember]` makes one and refuses to replace
  one. `luaug build --bump-version-code` raises `version_code` in
  `luaug.toml`, in place.
- **An installation exports every target** (ADR 0104 §6): the editor's folder
  carries `player/windows-x64/`, `player/linux-x64/` and `player/android-arm64/`
  and the Android Gradle template, so Linux and Android exports need nothing
  but the folder. `luaug android install-tools` installs a pinned JDK 17, the
  Android SDK (platform 35, build-tools 35.0.0, platform-tools) and Gradle 8.12
  into the user's LuauG folder -- no NDK -- and a second run installs only what
  is missing. `luaug build --status` says, as JSON, which targets this machine
  can export.
- **The editor exports** (ADR 0104 §4): **File > Export...** (Ctrl+Shift+B)
  and an Export button beside Play open a window with a card per target --
  ready, tools missing (with **Install Android tools**), or the phone on adb by
  model -- the selected target's settings written into `luaug.toml` as they
  change, Android signing with **Create keystore...** and a password it can
  remember on this machine, and a multiplayer selector that adds the two server
  cards. **Export** and **Export all** run `luaug build` as a worker process
  with a live step list, then a result card with the size and time, **Open
  folder**, **Run it** and **Install on phone**; the last ten exports per
  project are kept. Project Settings gains the App fields -- version, company,
  icon with desktop, taskbar and phone previews, fullscreen and resizable.

- **The editor plays a match** (ADR 0106): beside Play, a player count (1 to 4)
  and a *Dedicated server* box. With more than one player, Play saves the scene
  and starts a host (or a server) and its clients as separate windows, tiled
  on the screen, each one's output in the Output panel under its own name;
  Stop closes them all. The engine gains `--window=x,y,w,h`, `--label=` and
  `--log-file=` for it.

- **Attributes replicate** (ADR 0106, protocol 17): an attribute the authority
  sets on a replicated instance, a `Player`, a `Team` or `GlobalScriptService`
  reaches every replica that has it, and `AttributeChanged` fires there. **A
  peer must speak protocol 17**: this and the scene change are wire changes.
  `examples/24-scenes` gains a lobby whose `Ready` is an attribute.

- **The network from a script** (ADR 0106): `NetworkService:Join(address?)`,
  `Host(port?)` and `Disconnect()`, `State` (the new `Enum.NetworkState`) and
  the `Connected`, `JoinFailed` and `Disconnected` events. Joining replaces the
  scene with the server's and stops this machine's server code; leaving goes
  back to solo, the other players go, and server code starts again.
  `[network] server` in `luaug.toml` is where `Join()` goes with no address.
  `--host`, `--serve` and `--join=` are the same calls before the first tick.

- **Scenes at run time** (ADR 0106): `SceneService:LoadScene(path, data?)`
  swaps the whole scene -- world, services' contents and settings, its own
  `src/scenes/<scene>/` code -- between two ticks, with `SceneLoading` and
  `SceneLoaded` around it, `GetLoadData` in the new scene and `CurrentScene`
  saying where the game is. `GlobalScriptService` and a `ScreenGui` with the new
  `KeepOnSceneLoad` stay. In a match the authority changes scene and every
  client follows without reconnecting (protocol 16: a `SceneChange` message);
  a client's own `LoadScene` is refused. `examples/24-scenes` is a menu and an
  arena.

- **Touch input and a game on an Android phone.** A finger on a touchscreen is
  `InputBegan` / `InputChanged` / `InputEnded` with `Enum.UserInputType.Touch`,
  and the new `InputObject.TouchId` says which finger; a tap shorter than a
  frame still begins and ends, and the mouse the system makes out of a finger
  is not a second input. `examples/20-platformer` has on-screen controls that
  appear on the first touch and drive its actions through `SetVirtualState`.
  `scripts/android-player.ps1` builds a project into an APK -- the player host
  as `libmain.so`, the engine's content and the game, extracted to the app's
  storage on first launch -- and installs it with `-Install`; it plays on a
  Galaxy S25 Ultra. Textures on Android are uncompressed RGBA until the
  transcoder targets ASTC.
- **`UIService.ScreenOrientation`** (`Enum.ScreenOrientation`: `LandscapeLeft`,
  `LandscapeRight`, `LandscapeSensor` -- the default -- `Portrait`, `Sensor`):
  which ways up a game may be held, applied to the phone the moment it changes.
  **`Camera.ViewportSize`**: the size the world is drawn into, in pixels,
  following a resize or a turn.

- **A part may change its material's surface shader parameters** (ADR 0091),
  one by one, as it changes `Color`: a material names them in
  `instanceParameters`, and `BasePart:SetMaterialParameter`,
  `GetMaterialParameter` and `ClearMaterialParameter` take their names. The
  values are saved with the scene, copied by `Clone` and edited in the
  Properties panel; they are not replicated, and a texture is not one.
  `GetMaterialParameter` answers `nil` for a shader parameter nothing sets.
- **A lost graphics device is survived.** A shader that hangs the GPU made the
  driver reset and the engine crash inside SDL; now the engine says so, the
  editor offers to save and restart, and the surface shaders that were on
  screen are held back until they change. A game says so and closes, with
  exit code 5. `--simulate-device-loss=N` loses the device on frame N, and
  `--surface-cache=DIR` moves the compiled-surface cache.

### Changed

- **Every project recompiles its content once, on its next open.** Models
  now keep their morph targets (shape keys) when they are compiled (ADR 0196,
  its first stage: they are imported and stored, and not drawn yet), and a
  model compiled before that has none in its cached form. It is one open --
  seconds for a small project, under half a minute for one of six hundred
  sources -- and the opens after it are as quick as they were. Nothing is
  wrong with the project and nothing needs doing.
- **An Android package declares the permissions its game uses, not all of
  them** (ADR 0104, amended). `INTERNET` and `ACCESS_NETWORK_STATE` when a
  script names `NetworkService` or requires `@std/net`, or the project
  configures a network; `VIBRATE` when one names `HapticService`. A game that
  plays alone declares none. **If your game reaches the network through a
  name built at run time, set `[export.android] internet = true`.**
- **An Android export refuses a native library not laid out for 16 KB
  pages**, which a store requires of an app that targets Android 15: the last
  step reads every library in the package. The engine's own passes.
- **The lean ground** (ADR 0175). At `Low`, and at `Medium` on a phone, a
  terrain's material is drawn from one read of each layer's maps and one more
  of its colour at the far scale, with one noise a pixel where there were
  eleven: on the owner's phone the terrain's fragment was two thirds of the
  frame. `High` and `Ultra` draw the ground as they did.
  `[graphics] terrain_surface = "full"` (or `"lean"`) says otherwise for a
  project, and `--terrain-surface=` for a run.
- **The forward pass no longer shades what is hidden, on a phone** (ADR
  0174). What the depth prepass drew is now drawn without writing depth
  again, which lets a tile-based GPU test depth before it runs the fragment;
  it could not, and shaded every hidden fragment in full. The picture is
  unchanged.
- **The first frame of play no longer makes the renderer's pipelines** (ADR
  0176). The world is drawn behind the loading curtain for the frames before
  it lifts, and the pipelines of particles, decals, name tags and their kin
  are made there: on a phone the frame after the loading screen took over a
  second, and a first spark or a first name tag another hitch.
- **A phone's textures are compressed** (ADR 0180). A compiled texture was
  sent to an Android GPU uncompressed -- four bytes a pixel -- because the
  only block formats the engine knew were a desktop's. It is ASTC there now:
  a quarter of the memory. Nothing changes on a desktop.
- **A ground for a phone, compiled apart** (ADR 0179).
  `[graphics] terrain_surface = "fast"` draws the terrain with a shader that
  holds nothing but what it does: at most four layers a pixel by their colour
  alone, the mesh's own normal, diffuse light through four shadow taps. The
  one shader for every ground keeps each kind behind a switch, and a phone's
  driver can charge for all of them. A phone's Low and Medium draw it: on the
  phone it was measured on, sixty frames a second where the lean ground held
  fifty-six and forty-one. `[graphics.android] terrain_surface` says
  otherwise. `"flat"` is one colour, for measuring.
- **Walking onto new ground no longer costs a frame** (ADR 0181). A terrain's
  colliders were meshed on the worker threads and then had their collision
  shapes built one after another on the main one: up to four a tick, and the
  slowest frames of a phone's run (17 to 27 ms each). The shapes are built on
  the workers too. The simulation is the same: the bodies are made in the
  order they were.
- **A phone that stepped down to thirty frames a second tries sixty again**
  (D559). The pacing took the CPU's time of a frame at thirty -- two ticks of
  the simulation -- as what a frame at sixty would cost, found it did not
  fit, and never tried. The log's line for a step up says both numbers.
- **At its display's own rate a phone's frame is paced by the game's cap as
  well as the display** (D560), as it is with `adaptive_frame_rate` off: on
  the phone it was measured on, the display alone left three times the frames
  over 33 ms.
- **`--frame-stats` counts the frame cap's wait as waiting** (D561), under
  the scope `wait.cap`. It was reported as drawing on the CPU.
- **A timed run says how fast the GPU was running** (ADR 0171, amended):
  `--gpu-pass-times` times the same fixed work with every frame and reports
  it as `clock`. A phone slows its GPU under a light frame, by the same
  factor for every pass, and two runs could not be compared without knowing
  it.
- **A build does not ship a surface shader it could not compile** (D572).
  `ludwerk build` stops, naming the shader and the compiler it looked for,
  where it packed the source and said nothing -- and every copy of the game
  drew the error surface. A compiler that fails without an error of its own
  now says how: not started, or ended with a code (a library missing beside
  it), with its path.
- **A build no longer ships the settings of whoever last ran the project**
  (D573): `.engine/settings.json`, written by a run from the project's folder,
  went into `game/` and was every player's language, window and quality.
- **A model's own pictures no longer cost the frame it is first shown in**
  (D571): they are made ready on the worker threads, as loose and compiled
  maps already were. 23 ms of a phone's frame for one character.
- **A swarm the host has disabled stands still on every other machine**
  (D570). `Swarm.Enabled = false` on the authority was not told to a replica,
  which went on carrying every agent along its last walk and being corrected
  back: a held horde shook on a joined machine for as long as it was held.
- **A sealed game checks its own files** (ADR 0183): a pack cut short or
  changed is "this game's files are damaged", said in a dialog, where a game
  read as far as it could.
- **A phone whose GPU lacks a feature the engine never used is no longer
  refused** (D567). The graphics device was asked for clip distances,
  anisotropic filtering and a first instance in indirect draws, none of which
  anything here uses, and a GPU without one -- a large family of phones' --
  closed the game as it opened. They are not asked for; depth clamping is done
  without where a GPU has none.
- **A game that cannot start says why** (D568): a dialog with the error and
  where the log is, on a phone and for a game opened by a double click. And on
  Android the log is where a tester can reach it (D569):
  `Android/data/<id>/files/engine.log`, with the run before's beside it.
- `--gpu-least` draws on a desk as a GPU with no depth clamping does;
  `[debug] astc = false` (or `--no-astc`) sends textures as RGBA to a GPU that
  samples ASTC, to tell a fault of that path from another.
- **Many of one mesh are drawn only where each is needed** (ADR 0182). A
  forest, a horde -- anything the renderer draws as one instanced run -- was
  drawn whole into every pass that wanted any of it: into each shadow cascade,
  and into the camera's passes for one member on the screen. Each pass draws
  the members it needs. In a horde game's scene that is three fifths fewer of
  those triangles for the camera and from a third to four fifths fewer for
  the shadow maps, and no picture changes.
- **A join through a relay is carried whole** (D566). The relay's allowance
  for a carried player held one second of its rate, and a join's first second
  is several: a third of it was dropped and sent again. It holds eight;
  `engine-relay --burst=SECONDS` says another.
- **Frames at the simulation's own rate run one tick each** (D565). A display
  at sixty and a simulation at sixty could fall into running no tick in one
  frame and two in the next, for as long as the two clocks stood a certain
  way: twice the simulation's cost in half the frames.
- **Text with a stroke no longer costs a frame the first time it is shown**
  (D563). A letter's outline was made by trying every point of the stroke's
  shape at every texel of the letter -- 16 and 32 ms on a phone in the frames
  a HUD and a panel first appeared. The same outline, byte for byte, is made
  about six times faster. `--frame-stats` names the glyphs made in a frame
  (`ui.glyphs`).
- **A compiled texture no longer costs the frame it is first drawn in**
  (D564). An exported game's maps were made ready for the GPU on the frame
  thread, one at a time: 25 ms each on a phone. They are made ready on the
  worker threads, several at once, as loose images already were.
- **A host whose relay could not be found by name finds it later** (D562).
  `NetworkService.RelayState` was `Unreachable` for the whole session when
  the name did not resolve as the host opened; it is looked up again every
  half minute, and `JoinCode` arrives when it answers.
- **The full ground's shader is compiled only by a game that draws with it.**
  It took 1.3 s to make on a phone, during the first frame after loading,
  whichever ground the game used.
- **A match is joined over the internet by a code, with no port opened and no
  VPN** (ADR 0178). `NetworkService:Host(port, { Relay = "host:port" })`
  registers the match with a relay -- `engine-relay`, a small program on one
  UDP port that the game's maker runs -- and `NetworkService.JoinCode` is the
  eight characters a friend types into `Join`. The engine finds the path: the
  same network, across the internet each to the other, or through the relay
  where nothing shorter opens; `GetStats().Path` says which. `RelayState`,
  `RelayStateChanged`, `[network] relay`, `--relay=`. A match's own bytes are
  unchanged.
- **A phone's frames are paced** (ADR 0173). A game that names no
  `max_frame_rate` is capped at sixty on a handheld, and its frames are held
  to the highest rate they fit that the display shows evenly -- 60, 40 or 30
  on a display at 120 -- each frame shown for as long as the last. The
  display is told the rate. `[display] adaptive_frame_rate = false` turns the
  stepping off; `max_frame_rate = 120` lifts the cap.
- **`Low` and `Medium` filter the sun's shadow with fewer taps** (ADR 0172):
  four and eight, where every level took sixteen. `High` and `Ultra` are the
  picture they were. The filter follows `ShadowQuality`.
- **A pass with nothing to draw is no longer begun** (ADR 0172): the sun's
  shadow atlas with no cascade, the local lights' atlas with no casting light,
  the clear of a feature that is off, the forward pass reopened after
  particles with no world interface. Fewer passes a frame at every level; the
  picture is unchanged.
- **On a phone, bloom is three levels and contact shadows start at `High`**
  (ADR 0172). `contact_shadows = true` under `[graphics.android]` turns them
  back on at `Medium`.

- **Another player's character is met where the server has it** (ADR 0163).
  On a client, a character collided with other players' characters where they
  are drawn, a little in the past: running beside another player, it was
  stopped by somebody who was not there and corrected at every snapshot. It
  now meets them where the server is expected to have them. They are still
  drawn, and found by a raycast, where they were. Nothing to change in a game.
- **A quality level sets how much foliage is drawn** (ADR 0147): half at low,
  three quarters at medium, all of it from high. A project at low or medium,
  and every handheld -- which starts at medium -- draws less than it did;
  `[graphics] foliage_density = 1.0` puts it back.

- **A child that is not `Visible` keeps no room in a layout** (D478): a hidden
  child of a `UIListLayout` left a gap where it had been, and a parent with
  `AutomaticSize` stayed as large as what was hidden. An interface that hid a
  row and relied on the gap changes.
- **A `ScrollFrame`'s canvas is never smaller than the frame** (D479): with
  `CanvasSize` zero on an axis, a child sized as a fraction of its parent was
  nothing long on that axis, and is now as long as the frame.
- **A gamepad is connected a frame after a game starts** (D476): the platform
  looks for gamepads once the first frame is on the screen, so a window is
  never later for it. `InputService.GamepadAvailable` at a script's first line
  is false; connect to its changed signal.

- **Protocol 30**: a part's `Shape` and a mesh part's mesh travel (D424). A
  client and a server must be of the same build.
- **`ColorCorrectionEffect.Contrast` is a power about the frame's average**,
  where it was a line through it (D427): the same number is gentler on the
  dark half of a picture, which no longer goes to black. A scene graded with
  contrast looks different; one at `Contrast = 0` is unchanged.
- **What is see-through casts no shadow** (D423).
- **`BlurEffect.Size` has a range**, 0 to 100, and raises outside it like the
  others; `Water.Size` takes a width of zero, since a lake's outline is its
  points.
- **`SunRaysEffect.Spread` past a half widens the sky that can shine**, so the
  rays do not go out when the sun's disc is behind one tree's crown.
- **The final picture is dithered by one 8-bit step**, by pixel and never by
  time, so a glow fading into a dark sky shows no contour rings.
- **"Two sources for one world" is said only for a thing that is there
  twice**: something a script made that the scene already holds by class and
  name -- not for a camera or a bullet a script parents to `Workspace`.
- **The terrain's level of detail is chosen in rendered pixels**: at
  `render_scale = 0.75` (the `low` preset) or under a cap, distant ground is
  as coarse as that picture can show, where it was refined for the display's
  full resolution.
- **Frames are paced** (ADR 0147, stage G0). A window's frames wait for its
  display (`[display] vsync = true`, the default), may be capped
  (`max_frame_rate`), and are drawn ten times a second while the window is
  unfocused or minimised (`background_frame_rate`; 0 turns it off). A
  minimised window used to be drawn as fast as the machine went. The editor
  is paced the same way. `--[no-]vsync`, `--max-frame-rate=N` and
  `--background-frame-rate=N` on the command line. The simulation is
  untouched: a throttled window runs every tick it owes.
- **Terrain is drawn as far as the camera sees** (D403): out to
  `Camera.FarPlane`, where it stopped at 4 096 m whatever the camera. A world
  wider than the default 5 000 m needs a camera with a larger `FarPlane`.
- **Sculpted terrain no longer crosses itself** (D402): where two surfaces
  passed within a voxel of each other -- a thin wall, two hollows side by
  side -- the ground showed faces meeting in an X and black triangles until it
  was smoothed. Each surface has its own vertices there now. Ground without
  such places is drawn exactly as before.
- **A second run from the same folder keeps its own log** (D401): where
  another run is writing `engine.log`, this one writes `engine_2.log` (up to
  `engine_9.log`) instead of logging to the console alone, and the first
  run's log is never rotated out from under it.
- **Smoothing terrain no longer lags** (D400): a smooth stamp at radius 8 costs
  about 2 ms instead of 55 to 83, and the editor smoothing the owner's place
  holds p99 under 18 ms where it stalled for half a second. Wide paint strokes
  cost half what they did. The ground every brush leaves is unchanged.
- **Anything placed in the world can be clicked and moved in the editor**
  (D399): a `Water` by its surface, a `Terrain` by its ground, and a
  `WaterPoint`, a `Decal` and a `NavigationLink` by a marker; the move gizmo
  moves each by how it is placed -- a river by all its points, a water's height
  as its `SurfaceLevel`, a decal relative to its part -- and the scale gizmo
  sizes a lake and a river's width. A click on the ground selects the terrain.
- **The editor speaks through the catalog** (ADR 0145, supersedes ADR 0046's
  exemption): every panel, menu, dialog, the command palette, the status line
  and the undo history's labels read their words from `i18n/en.json`, so the
  editor can ship in another language. In English it reads as it did; window
  and dialog ids are unchanged, so a saved layout still applies.
  The repository's i18n lint holds the editor's raw words at zero.
- **A terrain far away is the same shape, at lower resolution** (ADR 0140,
  terrain audit T2). A coarse level is the level-0 surface gathered: a coarse
  cell's vertex is the point nearest the level-0 surface inside it, its material
  and paint the surface's, and a slab thinner than a cell thins to a sheet
  rather than vanishing. Flat ground no longer moves between levels.
- **No crack or skirt where two levels meet** (ADR 0140): a node takes the
  vertices of the coarser nodes beside its sides and corners, and the skirts are
  gone. A seam's vertices slide onto their parent's together in every node that
  draws them, so a change of level shows neither a pop nor an open seam.
- **The level is chosen by projected error**: a node shows its children where
  what it gets wrong would cover more than 4, 3, 2 or 1.5 pixels at quality low,
  medium, high and ultra -- measured from its own box, height included, with
  hysteresis. Flat ground stays coarse; a bump, an edge or paint is drawn finer.
- **The sky term is the drawn level's own**: air under an overhang is air, and
  a feature a level does not draw no longer shades the ground under it. Nor
  does a coarse column's width draw dark streaks down a steep flank or dark
  blocks on a far ball.
- **Terrain is built off the main thread** (ADR 0141, terrain audit TA14): a
  few workers build every mesh the ground wants, and a node is drawn only with
  a mesh built for the levels drawn beside it -- the ground as the frame before
  drew it meanwhile -- so a change of level, an edit or a streamed cell is drawn
  a frame or two late and whole, never with a seam open. The terrain flight
  over the owner's place, at 60 frames a second: a p95 of 5 to 6 ms and a p99
  of 8 to 9, where building in the frame had been 22 and 33.

- **A terrain's materials come from Content** (D330), as in other engines: the
  Paint panel's `+` picks one of the project's materials, a material dragged
  from Content onto it adds it and onto a swatch replaces that layer, and a
  swatch's right-click replaces, opens or removes it. The panel no longer makes
  materials; *New Terrain Starter Materials* is in Content's right-click menu.
- **`Terrain:PaintBall`'s `Falloff` is how much of the radius fades** (D328):
  the inner rest is the whole `Strength` and the fade ends at nothing on the
  rim -- it was a mix of a hard brush and a soft one. A `Blend` or `Erase` stamp
  takes its share of what is left, so a soft stroke keeps a soft edge however
  many stamps pass.
- **The editor is laid out as a game engine** (the owner, 2026-09-27: the look
  may be the code editor's, the layout has to be an engine's). The Explorer
  alone on the left, Properties the full height on the right with Stats a tab
  beside it, and along the bottom, under the tree and the world, the Content
  browser, the Console and Run and Debug. Every open script is a tab beside the Viewport -- and
  one that would open undocked, from an older layout, opens there too. Play,
  pause and step sit in the middle of the toolbar, the transform tools on the
  left, and the snap steps, the tools and Export on the right. The browser
  shows the project's folders beside a grid wide enough for names. Layouts
  saved before are rebuilt once.
- **The material editor opens beside the Viewport, the Viewport's size**: a
  large preview turned by dragging, on a ball, a block or a cylinder, beside
  its fields in the Properties grid -- Surface, Lighting, Textures, Surface
  Shader and what a part may change. A texture slot shows its picture, takes a
  texture dragged onto it and clears with one click. Closing with unsaved
  changes asks. The Export window opens beside the Viewport too.
- **Preferences is one window of one size**: a search over every setting, the
  pages in a tree down the left, the settings in the Properties grid.
- **Anything in the Content browser drags onto a folder to move there** -- a
  folder cell, the folder tree or a step of the path -- and every scene,
  stamp, material, the start scene in `project.toml` and the open world that
  named it follow. A rename does the same. A field that takes a file lights up
  only for a file of its kind.
- **A printed table opens in the console**, and the tables inside it,
  closed until opened; the line says what is in it rather than its address.
  The same line printed again from the same place is one line with a count, and
  a script's line names where it was printed, which a click opens.
- **The reference grid fades between its spacings** as the camera climbs,
  instead of jumping to ten times the spacing at one height.
- **The collision wireframe is drawn where the parts are drawn**, between two
  ticks, and coloured by how a body moves.
- **Properties, from the owner's pass over it**: categories and the
  properties in them are in alphabetical order, a category is a heading that
  reads as one, Attributes and Tags are rows of the same grid, a number field
  works out what is typed into it (`0+2`, `1/2`, `90/4`), and an enum shows
  its item's name closed and its whole name open. Stats is drawn in the same
  grid. An instance whose `Enabled` is off is dimmed in the Explorer.
- **A material's textures tile by size on a part** (`TileSize`, 4 m by
  default): a face shows as many repeats as it is long, so the edges of a long,
  thin slab are no longer the whole texture squeezed into a strip. 0 stretches
  each texture over the face, as before; a `MeshPart` keeps its file's UVs. The
  material editor shows it under Textures, and its preview ball is one repeat
  across.
- **A folder with something in it is drawn filled**, in the Content browser's
  grid and tree and in the Explorer.
- **The activity bar marks a panel that is open**, wherever it is: a click
  closes it as its own X would, and a click on a closed one opens it with the
  keyboard in it.
- **An Android export's orientation is the start scene's
  `UIService.ScreenOrientation`**: `[export.android] orientation` is gone. Two
  answers to how the phone is held turned the screen the moment the game
  started whenever they differed.
- `SetMaterialParameter` with a name no built-in field has, on a part whose
  material does not declare it, raises *not declared* rather than *not a
  parameter*: such a name may be a surface shader's.

### Fixed

- **A skinned model exported without normals is weighted as its file says**
  (D606). Such a model is given flat normals, which makes a vertex of every
  corner -- and the joints and weights were not carried to the new vertices,
  so each took another's bones. It bent wrongly, silently.
- **A model is compiled again when a file it reads beside itself changes**
  (D604). A glTF's external images and buffers were not part of what made it
  stale: repaint a model's texture and the editor went on drawing the old
  one. They are now, by their bytes.
- **Opening a project no longer recompiles it for a material that was
  saved** (D597). Any material file saved or touched sent every source of
  the project back to the compiler -- a minute and a half for a project of
  six hundred sources, with nothing to compile. Sources are recompiled when
  their own bytes change, or when a material starts or stops naming an image
  as a normal, roughness or height map. And when there is something to
  compile it is compiled thirty-two sources to a pass: a first open of that
  project went from four minutes to under thirty seconds.
- **A game on Windows 10 starts on Vulkan** when the machine's strongest
  card can be drawn on through it (D596). One
  player's Windows 10 machine drew a game's scene dark and with no interface
  through Direct3D 12, and right through Vulkan; the Direct3D cause is still
  open. `--gpu=direct3d12` asks for the old behaviour, and Windows 11 is
  unchanged.
- **A pipeline or shader the graphics driver refuses is an error in the
  log**, with its name and the driver's reason (D596). It was counted and
  not said, so a picture with a pass missing left a log that read as a
  healthy run.
- **A game no longer sets off Windows' Controlled Folder Access at its
  start** (D595). The engine asked the system where Documents is on every
  start -- for the editor's launcher, which a game has none of -- and that
  question is reported as the game trying to change the folder. A game asks
  nothing of Documents now.
- **A game that cannot write beside its executable keeps its log in its own
  folder** (D595): `%APPDATA%/<company>/<game>/logs`, beside its saves, with
  its crash files and the record `RunService:GetLastRun` reads. It was the
  engine's folder.
- **`UIPadding` insets an element's own text** (D594). The words of a
  `TextLabel` or `TextButton`, and a `TextInput`'s text, caret and
  placeholder, are placed, wrapped, cut and scaled inside the element's
  padding, as its children are. They were drawn in the whole rectangle, so a
  label aligned to the left sat against the edge. **If you padded a text
  element and worked round this with a frame inside it, the frame is no
  longer needed; if you relied on the words reaching the edge, they move in.**
- **A game on a phone whose shared storage has no file locks writes its log**
  (D593). On such a phone the log was nine empty files: the lock that tells a
  second run from the first could not be taken, and that was read as "another
  run has it". With no log there was nothing for a tester to send and no
  record for `RunService:GetLastRun` to read.
- **The engine's mud, grass and rock no longer show their repeat as a grid**
  (D592). Each carried a feature a quarter of its tile across -- mud's were
  puddles, nearly mirrors -- which a field of it showed once a tile, most of
  all on a phone, where a layer is read once and nothing hides a repeat. All
  three are finer now, and a puddle is wet rather than a mirror. **A terrain
  that uses them looks a little different**: the same colours, smaller
  features.
- **Two players' characters that block each other no longer rubber-band on a
  machine that joined** (D590, ADR 0163 amended). A friend walking into a
  standing player pushed that player's own character away on his screen and
  was corrected every other frame; a player following a friend who stopped
  walked through him and was pulled back out. Another player is now met
  anywhere he may be -- where he was last known to be when going away, as far
  forward as he may have got when coming, never inside the player's own
  character. Measured in two processes at 0 and 80 ms each way: no step back,
  no frame walked through. Nothing for a game to set.
- **`ludwerk check` and `ludwerk setup` need no GPU** (D591): checking a
  project's scripts as the editor does, and writing its scene's tree as
  types, open no graphics device. On a build machine the second had failed
  without a word.
- **An installed `ludwerk` runs its own engine** (D589), not the one in a
  build tree `ENG_BUILD_ROOT` names.
- **A project `ludwerk check` calls clean opens clean in the editor** (D583
  to D586, ADR 0093 amended). The script pane followed a `require` only
  through the tree, so a project that requires by path --
  `require("../shared/Match")` -- had every type of every module unknown; it
  follows a path now, by the rule the engine runs one by. Its checker ran
  Luau's new solver with the fixes made since switched off, and runs with
  them on. Its own lint called a module required for its types "never used",
  and `NumberSequence`, `NumberSequenceKeypoint`, `ColorSequence`,
  `ColorSequenceKeypoint` and `scene` unknown globals. On a game of a hundred
  files: 193 errors and 58 warnings, to none.
- **`ludwerk check` checks a project as the editor does**, after the
  analyser: every scene's scripts through the editor's own checker
  (`engine-host <project> --check-scripts`). Its errors fail the check; its
  warnings are printed. A require by path that names no file is one of them.
- **The engine's definitions are one file every analyser reads** (D588).
  `.engine/types/engine.d.luau` typed `Instance.new` in a way the new solver
  refuses, so `ludwerk check` analysed a rewritten copy and an editor
  extension pointed at the file knew no engine class. It is written the way
  the new solver reads; `.engine/check/` is no longer made and may be
  deleted. A new project carries a `.vscode/settings.json` that names the
  file, the standard platform and the new solver.
- **An installed engine's `ludwerk setup` writes the modules a script requires
  by name** (D587): `.engine/types/engine/` and `.engine/types/std/`, which
  the package had left out.
- **A machine that joined a match pays for what moves, not for what there
  is** (D582). Every snapshot it read every replicated instance it holds to
  find which had changed; it reads the ones that did. Measured with 2,700
  instances and sixty moving: 1.42 ms a frame of receiving down to 0.48.
- **A crowd of animated bodies costs a third of what it did** (D581). Bodies
  of a horde on the same rig and clip share the poses the others built, as
  they were meant to: two in three were being worked out again from the
  clip's keys every tick. Measured on 550 bodies: 0.63 ms a tick of posing
  down to 0.19. Nothing to change in a game, and every pose is what it was.
- **A host laying ground with nobody in the match no longer pays to tell
  nobody** (D580): a land made by a script at a match's start cost the host
  a frame of twelve milliseconds compressing changes no peer was there for.
- **A host no longer drops frames when somebody joins** (D579). On a match
  whose ground was made or changed by a script, the frame a friend joined in
  cost the host twenty milliseconds and more -- the whole ground compressed
  for the newcomer inside one frame. It is compressed off the frame now, and
  the newcomer begins a tick or two later.
- **A picture drawn small in the interface is no longer ragged** (D578). An
  `ImageLabel` or `ImageButton` showing a picture well under its own size --
  a 256-pixel icon in a 17-pixel slot -- sampled four texels of the hundreds
  each pixel covers, which broke outlines into steps and turned fine detail
  to noise. A picture now goes to the GPU with its smaller levels and is
  drawn from the ones nearest the size on screen. Nothing to change in a
  game; a picture costs a third more memory. An icon drawn at its own size
  looks as it did.
- **A turned part in a run of identical parts is lit from the right side**
  (D577). Parts the engine draws together as one instanced run -- the same
  mesh and material many times, and every enemy of an animated horde -- had
  their normals turned by the inverse of their rotation: a crate turned a
  quarter turn was bright on its shaded side. A part not turned showed
  nothing. **If a surface shader of yours turns its normal back to work
  round this, take that out**: it now turns a right normal wrong.
- **A `ViewportFrame` is no longer a white square the frame it is first
  shown** (D575). Each time one became visible it drew solid white for a frame
  before its instances -- every preview in a menu, each time the menu opened.
  A frame with no picture yet shows its own background.
- **A character moving fast over terrain no longer stands still** (D574). A
  `CharacterBody` whose step in a tick was long enough to reach from one facet
  of the ground on to the next -- a dash, a mount, a knock-back -- could stop
  dead on open, walkable ground and stay stopped for as long as it was asked
  to move. A walk did not show it.
- **The sun's shadows under an orthographic camera** (D536). A camera looking
  down from more than about fifteen metres -- a strategy game's, a map's -- drew
  no shadow of the sun at all, and chose every lamp from the nearest slice of
  the light grid: a fragment's distance was read from a value that is 1 under
  an orthographic projection.
- **A shadow no longer stops where a cascade begins** (D537). What stood
  between a shadow cascade and the sun, outside the cascade's own sphere --
  a tower beside the camera, at a low sun, onto ground far away -- was left
  out of that cascade's map. Casters are now kept along the sun's light.
- `player.Character = nil` takes a player's character away, as the guide says:
  from a script it was refused (D538).
- **Text at any size costs a few sets of glyphs** (D539). A label whose
  `TextSize` is animated, or a `TextScaled` label in the world as the camera
  moves, asked for a new set of glyphs at every size, filled the glyph cache
  and had it emptied -- every label rasterised again, and a warning each time.
  A size is now rasterised at the nearest of a few sizes at or above it and
  drawn smaller; text at a whole number of pixels up to 64 is drawn exactly as
  before, and text over 128 pixels is drawn up from 128.
- **Hidden interface no longer reads `AbsolutePosition` and `AbsoluteSize`
  as infinite or NaN** (D551). Under a `ScreenGui` with a `ReferenceHeight`,
  on a window of another height, the rectangle of anything not `Visible`
  grew at every layout. What is hidden keeps the rectangle it last had.
- **A phone is not left at thirty frames a second** (D558). The pacing of
  ADR 0173 judged a launch's long frames, stepped down on a fifth of one
  second, and could not step back up: a phone that held sixty was put at
  thirty three seconds in, for good. A rate is now given up only when more
  than three frames in ten are late two seconds running; nothing is judged in
  a start's first four seconds or round a long frame; the rate above is tried
  again once the CPU's part of a frame fits it; and the display is asked for
  the game's cap rather than the rate held. The log says why at every change.
- **A second finger presses a button** (D557). The interface heard one finger,
  the first down: with a thumb resting on a game's own stick, no button
  anywhere on the screen could be pressed. Every finger now presses what is
  under it and fires its `Activated`; hovering, dragging and scrolling stay
  the first finger's.
- **A streamed world arrives however small its time budget** (D556). A frame
  whose budget was already spent landed no chunk at all; on a slow machine
  that could be every frame, and with a budget of zero it was. One chunk a
  frame is now always landed, and the budget limits the rest.
- **A panel no longer opens as a white slab** (D555). An image whose picture
  had not arrived yet drew a flat rectangle in its `ImageColor3`; it draws
  nothing of itself now, and the element's own background shows meanwhile.
- **A model the project's cache had lost is compiled again** (D554). A
  project opened by two processes at once could be left with a model that
  "has no compiled form" at every open after, until its cache was deleted.
- **Name tags and signs in the world no longer flicker** (D553). A
  `BillboardGui` or `SurfaceGui` could be drawn for one frame in black or in
  another label's letters, each time the glyph cache filled.
- **Large text no longer stalls a frame every minute or so** (D552, ADR
  0169). On a dense screen -- a phone, a 2160-line window -- the glyph cache
  filled and was emptied whole, and every glyph on the screen was made again
  in one frame of 60 to 110 ms. It grows a page at a time now, and gives up
  only what has not been shown.
- **An attribute or a tag that changes no longer sends everything its
  instance carries** (D549). One attribute written every tick on an instance
  with eighteen sent all eighteen, by name, every snapshot -- most of what a
  joined player received in a game that wrote a facing or a timer each tick.
- **A moving part under TAA and FSR 2 no longer smears in patches** (D548).
  On about half the face of anything that moved, the temporal pass was told
  the pixel had moved as the background behind it and looked for it in the
  last frame in the wrong place. The part's motion is now drawn at exactly
  the depth the picture's depth pass wrote.
- **A list that scrolls down no longer grows a bar across its bottom** (D547).
  At some window sizes -- so, often, after a resize -- a `ScrollFrame` with
  nothing to scroll sideways drew a horizontal bar the length of the frame. A
  bar is also drawn only along an axis `ScrollingDirection` lets a hand scroll.
- **Ogg Vorbis plays** (D546). A `.ogg` as `Sound.Content` played the
  placeholder tone and said it could not be decoded, though the manual named
  the format: no Vorbis decoder was compiled in. It decodes now, streams when
  it is long, seeks, and loops at the stream's own last sample with no gap.
- **Collision groups reach every client** (D545). A part's `CollisionGroup`
  and the server's table of groups stayed on the server, so a character a
  client predicts collided with parts the server's walked through, and was
  corrected every snapshot while it touched one. Both replicate now: groups
  registered and set on the server are the same on every machine.
- **A new character no longer sends the whole glyph atlas to the GPU** (D543):
  the rows it was written on, a seventieth of it.
- **A long frame's warning is about that frame** (D540): "Frame took N ms"
  split the frame before the long one into its phases, so a tick that stood
  still was reported as a simulation that took nothing. `--frame-stats` names
  what arrives in a frame and each stretch of its drawing.
- **Terrain and grass arriving no longer hold the frame** (D541, D542, D544).
  A terrain node re-stitched because the level beside it changed was built in
  the frame that found it, as a brush stamp is; grass was grown with the frame
  waiting for it; and the engine's own terrain textures were drawn in the
  frame that first asked for them. A map's first second, and a camera moving
  over ground with grass on it, had frames of 30 to 50 ms from these; each is
  now done on the worker threads and shown when it is ready. An edit -- a
  brush, a script changing the ground under a player -- is still shown in the
  frame that makes it.
- **A window held by its title bar no longer stops the game** (D535). On
  Windows, holding or dragging a window's title bar, sizing it, or opening its
  menu stopped the whole main loop until the hand let go -- and a host that
  moved its window froze the match for everyone in it. The loop now runs
  through it at about sixty frames a second: simulating, sending and drawing.
- **Predicted touches on triggers, and replays through them** (D524, D525): a
  character begins and ends touching a part that does not collide -- a jump
  pad, a teleport, a zone -- by standing in it, for `Touched` and for
  `BindToPredictedTouch`; and a launch or a move a predicted touch makes is
  stepped again by a correction that answers its tick, rather than lost. And
  the authority never skips ticks of input of a player whose predicted steps
  keep state -- a cooldown counted one tick for several (D526).
- **The 2D layer under scaling and anti-aliasing** (ADR 0158, amended). A
  picture of sprites alone is drawn at the window's resolution whatever
  `RenderScale` says, upscales nothing and is never jittered, so a phone's
  three-quarter scale no longer softens a 2D game. Pixel art among 3D surfaces
  keeps its pixels: it is placed without the jitter, passed through by TAA,
  FSR 1 and RCAS, and scaled by the nearest texel. The 3D around it keeps its
  temporal pass rather than falling to SMAA. `imgprobe` checks a region's
  palette, and a region against another image.
- A view into a texture -- a `ViewportFrame`, a sub-world, a camera's
  picture -- is drawn at its frame's own size whatever `RenderScale` says: the
  first-person weapon of a game at a reduced 3D resolution came out in hard
  blocks (D528).
- `VSync` off is an immediate present, torn mid-refresh, as in every engine; a
  mailbox only where the backend cannot tear. A machine was held to its
  monitor's refresh with VSync off. The log names the present mode and the
  driver (D529).
- A corrected replica replays its ticks with the live step's 64-bit time, not
  a 32-bit copy: a game counting seconds in its predicted step was corrected
  at every snapshot while its player was in the air (D527).
- A snapshot no longer writes the authority's older velocity over the
  replica's own predicted character: a predicted step that read the body's
  speed was corrected on every fall (D530).
- Ground rewritten by the host in a match reaches every replica: an edit's
  message of more than a megabyte was refused by the transport and lost, and a
  friend played on ground the host did not have. Ground messages are now at
  most 256 KiB, and a refused reliable message is said in the log (D533).
- **A horde for a match** (ADR 0156, amended): `Swarm:SetTargets` sends each
  agent after the nearest of several targets -- every player -- and
  `Swarm:AddAgentAt` adds an agent with no body, for a server that simulates a
  horde its players draw.
- **A scene is shown once it has arrived** (ADR 0159). The loading curtain is
  raised for every scene, not only the first, and lifts once the meshes and
  pictures are in, the ground round the camera is meshed and no script holds
  it; behind it the ground is built flat out. `Terrain:IsMeshed` and
  `Terrain:WaitForMeshAsync` say when ground a script wrote is drawn, and
  `SceneService:HoldLoading` / `ReleaseLoading` keep the curtain up while a
  scene's scripts build its world.
- A character walking up a slope of terrain stays `Grounded`: it was carried
  off the crease between two facets and landed a tick later, every few ticks
  up every hill (D532).
- A point light wholly to one side of the view -- a lamp on a wall -- lights
  every tile it reaches: its inner edge was projected at its nearest depth
  only, and the wall was lit up to a straight vertical band (D531).
- **The local gate no longer ends other engines** (D523): it stopped every
  `engine-host` on the machine by name before its build -- a packaged game
  under test, another session's server -- with exit -1 and no log; it stops
  only its own build's now. And a project opened headless for the first time
  says what it compiles while it does.
- **A player is no longer corrected for a busy machine** (D498): a burst of
  input held up on the way made the server step some ticks twice; the client
  now says when its own clock dropped time, and only that re-anchors at once.
- **A game on a busy machine no longer freezes** (D500): its join took frames
  of seconds inside the graphics driver until the server dropped the player.
  Every game run is now scheduled ahead of ordinary work, as a server already
  was; the editor and a test run are not.
- **Tags replicate** (D501), added and taken off, as attributes do.
- **A copy of a stamp that changes one attribute keeps following the stamp's
  others** (D502), and its tags the same: a copy saves what it changed, not
  the whole set.
- **A stamp's parameter drives a material field** (D499) -- a `TeamColor`
  colours a hat -- and the Properties panel offers it on a material's fields.
- **Stamps' thumbnails show the stamp** (D503), where every one of plain parts
  was a grey square.
- **An open stamp shows its own markers and gizmos** (D504), not the scene's
  behind it.
- **Opening another stamp switches to it** (D505), asking first when the open
  one has unsaved edits; and the scene's undo history is still there after a
  stamp was open over it.
- **No patch of different shading follows the camera at a low sun** (D506):
  the far shadow cascades no longer darken a thin lit top -- a wall's cap.
- **A project opens fast, and never as a white window** (D507): a second open
  skips every source that has not changed (0.5 s where it was 3.8 s); a first
  open compiles while the window answers and says in its title what it is on.
  The editor opens no console window, and a game holds a dark frame until its
  first scene's meshes and pictures are in.
- **An animation loaded before its mesh's file arrived plays when it does**
  (D509): its `Length` was zero for good, and the figure slid about unanimated.
- **A ragdoll's feet and hands continue past their joints along the bone**
  (D510), where they were built pointing back into the shin and the forearm.
- **A ragdoll draws as a body** (D511): each limb's bone faces the way its
  joint does, where it faced the world's axes and a fallen figure's shin drew as
  a flat ribbon from the knee to the boot.
- **`PivotTo` every frame keeps a model where it is put** (D512): it drifted
  away exponentially, kilometres within a second. Moving a model onto the
  pivot it already has changes nothing.
- **A part nothing can meet costs the physics nothing** (D513): anchored, with
  `CanCollide`, `CanQuery` and `CanTouch` all false, it has no body -- a game
  that draws its figures from parts moves them for the price of a write. Turn
  `CanTouch` off on parts that only show.
- **A ragdoll moves the whole character** (D514): every skinned mesh of its
  `Model` that has its joints -- a shirt, trousers, hair -- by joint name, as an
  `AnimationPlayer` on the Model does. The other meshes used to stay in the air
  at their last pose while the body fell.
- **A glTF clip plays as it was keyed** (D515): `STEP` holds each key and
  `CUBICSPLINE` follows its tangents; both were read as linear, and a cubic
  spline's tangents played as poses. Meshes compile again (mesh format 3; 2
  still reads).
- **Idle skinned bodies cost nothing** (D516): a mesh whose tracks are all
  stopped is posed once, not every tick.
- **An exported game's characters are not boxes** (D517): `ludwerk build`
  compiles with the asset compiler of the player's own build and refuses a
  pair that do not agree on the mesh format; a mesh from a newer compiler says
  so instead of "malformed".
- **Bodies of one mesh share its collision hull** (D518): a part made a shot
  built the mesh's hull again, a quarter of a millisecond each; and the click
  detectors' pick no longer runs in a world with no detector.
- **Posing a skeleton costs a tenth of a microsecond a joint** (D520): a pose
  asked every track in the world whether it drove its mesh. And **poses are
  shared**: meshes of one rig at one moment of one clip copy one pose, bit for
  bit, and a crowd posed at a reduced rate shares at its clips' keys.
- **`ludwerk check` keeps the project's types this engine's** (D521): a copy
  left from an older engine is written again rather than reporting new classes
  as unknown.
- **Text is broken into lines once** (D519): a screen laid out again because a
  label moved no longer re-measures every label on it, and wrapping a long text
  is no longer quadratic in its length.
- **A server that loads its own scene again restarts every client's scene
  code** (D489), and `SceneLoading` fires on a client before its scene changes;
  a round restarted by `LoadScene` used to restart on the server alone.
- **`Bone.Transform` turns its joint**, and the joints below it follow (D491).
- **`PreRender` and `BindToRenderStep` run without a window** (D493), so a bot
  or a headless test reads input and moves its camera as a player's machine
  does; and **a `ViewportFrame` draws when the world has no camera** (D496).
- **A camera rig whose subject died holds still** (D497) where it raised every
  frame, and `rig:Destroy()` stops a rig for good.
- **`IsDescendantOf` and `IsAncestorOf` answer `false` for a destroyed
  instance** (D494), and a frame whose text filled the glyph store is built
  again rather than drawn with glyphs that had moved (D492).
- **Parts that vanished on Windows** (D484): an instanced run drawn after single
  draws on other meshes read every instance from the first under D3D12 -- an
  SDL backend defect, patched. A `MeshPart` with its own materials, or a new
  material, could take a wall or a crate stack out of the frame.
- **A part's `Material` in a scene once the scene partitions** (D483), a
  `CharacterBody` that is `Anchored` (D486), a connection a scene's script made
  through a global module after the scene ended (D487), a scene's stamp named
  as `Instance.stamp` names it (D488), and `HostFailed`'s reason without its
  catalog key (D485).
- **A part a player owned never moved on the server** (D482): handed over in
  the tick it was made, it was simulated from the origin by its owner and every
  place it was sent refused.
- **Joining a large world** (the netcode audit, NA1): a world of a thousand
  parts took 29 s to join on a loopback, and five thousand never did -- the
  first snapshot went as one unreliable message resent until a whole copy got
  through, and past a megabyte not at all. It goes reliably, in parts.
- **A client's own body at twice the place the server put it** on joining
  (N7, NA6).
- **The connection's clock is not the frame's**: the transport is serviced by
  a thread of its own, so a window in the background no longer reads a ping of
  100 ms nor sends its input six ticks at a time; a networked session in the
  background runs at sixty frames; ENet's throttle no longer drops snapshots
  and input for seconds after a spike.
- **A failed join kept nothing of the game** (N1); a full server answered
  nothing (NA8); `Host` on a busy port was silent (NA7); `Disconnect` lost
  what was sent just before it (NA9); `BindToClose` handlers could not reach
  the network (NA10); a client whose game froze stayed connected for ever.
- **A quick tap between two frames is a press** (NA25); a remote to an event a
  player does not hold no longer holds every other remote for five seconds
  (NA21); a server hitch no longer drops a client's remotes and newest input
  (NA22, NA3); a part a client's own script makes falls on that client (NA17);
  a teleport is drawn as one (NA15); a correction no longer pops (NA16);
  others are drawn as far in the past as the link needs (NA14).
- **A wheel spins past 47 rad/s** (K1): a ball, and a cylinder no longer than
  it is wide, up to 500.
- **A long frame's warning says where the time went** (N11).
- **A client is heard again after a long frame** (D480): a client that dropped
  simulated time once -- a hitch of a tenth of a second is enough -- was
  ignored by the server for the rest of the session: its character never
  moved again, or never stopped. The server now takes up a client's input
  again within a few ticks of its clock moving, corrects it once, and stops
  holding a key for a client that has said nothing for a quarter of a second.
- **A `ScrollFrame` is scrolled by the wheel and by a finger** (D477): it moved
  only when a script wrote `CanvasPosition`. The wheel over it moves its canvas,
  and a press that is then dragged scrolls it without pressing the row under
  the finger.
- **A gamepad works** (D476): the platform library was built without its
  joystick support, so no gamepad, in any build, ever reached the Input Action
  System. It is built with it now -- with haptics and the HID layer a modern
  controller speaks -- and `HapticService` drives a controller's motors.
- **A project's content is compiled once** (D474): the import cache's key was
  different in every run, so every start compiled every mesh and picture
  again. A project with six sky pictures started in ten seconds, and starts
  in a tenth of one.
- **A part is in every query from the moment it is in the world** (D475):
  `Raycast`, `Spherecast`, `GetBodiesInBox` and `GetBodiesInSphere` right
  after `Parent = workspace` find it.
- **A servo settles** (D470): a hinge's and a ball socket's servo is a
  critically damped spring at its responsiveness, in radians a second, whatever
  the arm it turns -- it swung round its target for seconds. Far from the
  target it travels at its speed.
- **A mover is solved with the joints its body is in** (D471):
  `AlignPosition`, `AlignOrientation`, `LinearVelocity` and `AngularVelocity`
  are motors in the solver, so an upright on the hips of a jointed body turns
  the body. `Stiffness` and `Damping` have units, and every property says
  them. A cap is now along each axis.
- **`BasePart.Mass` answers at once** (D472): from the part's shape, size and
  density, the moment it is in the world and the moment either changes. It was
  zero until the simulation had stepped.
- **A joint locked by equal limits reports no error** (D473).
- **A block world being built does not stall the frames that draw it**
  (D449): chunk meshes are made on workers and swapped in together, into
  buffers the chunks share, and a frame finds what changed without asking
  every chunk. One column built every four ticks: 1465 of 2389 frames over
  33 ms, to none.

- **Building a block world with a fluid registered costs what it costs
  without one** (D448): a write is queued for the fluid step only where the
  step would change something, and the step takes its budget without reading
  the whole queue. Twenty times faster to build, and no 50 to 170 ms ticks
  afterwards. A floor put under a pouring source now makes it spread.
- **A server that restarts no longer strands its clients** (D432), an
  animation track plays a second time (D438), and an Android game's window is
  the right way up from its first frame (D446).

- **A tap presses the interface on a phone** (D430): no button's `Activated`
  fired and no `TextInput` took focus, so the keyboard never opened -- every
  tap was pressed at the corner of the screen.
- **A game on a phone can use the network** (D431): an exported APK declares
  `INTERNET` and `ACCESS_NETWORK_STATE`. Without them `NetworkService:Join`,
  hosting and HTTPS all failed, with a message about a port in use.
- **A plain part faded past a half is drawn** (D423): at `Transparency` 0.5 it
  was stripes and above it nothing, an alpha test nobody had asked for.
- **A ball is a ball on a client** (D424): `Part.Shape`, and a `MeshPart`'s
  mesh, never reached a replica.
- **`--save-scene` keeps the terrain** (D425): it wrote a large scene without
  its ground and its streamed parts.
- **`StreamingService:LoadAreaAsync` waits for the ground too** (D426), which
  is how a script waits before `Terrain:HeightAt`.
- **Depth of field leaves glass and particles in focus sharp** (D428).
- **An emitter over its cap thins evenly** instead of pulsing (D429), and the
  log says it is capped.
- **A streamed part comes back under what it was authored under** (D422). In
  a scene large enough to be cut into cells, the parts of every `Model` left
  at the default `StreamingMode` -- and of every `Folder` -- came back under a
  `Chunk_x_z_layer` folder in `Workspace`, and the model stayed an empty shell:
  `model:FindFirstChild`, `GetChildren` and `Destroy` found nothing of it. A
  part now streams in and out under its own parent, an atomic model is made
  where it was authored, and the tree a script sees is the scene's whether or
  not the world is partitioned. A model a script destroys does not come back
  when its cell is loaded again. `Chunk_*` folders remain only in a generated
  world, whose cells no scene ever held.
- **While the game plays in the editor, the viewport is the game's** (D421).
  A click selects nothing and opens nothing; no brush, tool, drop or handle
  works on the running world; Ctrl+Z does nothing; a key typed into the
  Console or a script, and a click on a panel, do not reach the game; the
  editor draws none of its markers, grid or guides over it. **Escape is the
  game's and no longer stops it**: Stop is **Shift+F5** and the toolbar.
  **Shift+P** ejects -- the editor's camera, a click selects to inspect, and a
  pointer the game had locked is handed back -- and nothing structural can be
  changed until Stop. Stop gives back the undo history from before Play, where
  it cleared it, and leaves the unsaved mark as it was. The viewport wears a
  border while the game has it.
- **A tool is in hand only while its panel is the one on screen** (D421): the
  terrain brush, Blocks, Tiles and Water rest when another tab comes forward
  or the game plays -- no ring, no chip, and a click selects -- and are in
  hand again on coming back. An activity-bar icon brings a panel that is
  behind another tab forward, where it closed it.
- **A round brush paints a round edge** (D419): a hard `PaintBall` left a
  polygon with teeth a voxel across, and a stroke could leave an unpainted
  speck inside it. The rim of a stamp is now a ramp a voxel wide, so the edge
  drawn is the circle; `terrain:PaintBall` and the editor's Paint tool write
  slightly different covers at a rim than they did.
- **A fast body does not pass through the ground** (D417): every dynamic
  body is swept along its path on a step that moves it far, so a log at
  100 m/s lands on terrain, a block world or a thin part where it used to go
  through; the ground a fast body is about to reach is given its collider
  before it arrives; and a loose body that ends up inside a terrain's ground
  -- raised over it by an edit, or turned into it -- is put back on top of
  it. Two recorded determinism traces (`ragdoll`, `terrain`) changed for it.
- **A game fills a phone's display** (D416): on Android a game is immersive --
  the status bar and the navigation pill hidden until swiped for, the frame
  drawn under the camera cutout -- whatever `[window] fullscreen` says, which
  is a desktop's key. `UIService.SafeAreaInsets` says where a HUD may go, and
  a `ScreenGui` lays out inside it by default.
- **A file saved with a UTF-8 byte-order mark is the same file** (D415):
  `ludwerk build` refused such a `project.toml` at its first line, and a
  script saved that way did not compile. Notepad and Windows PowerShell's
  `Set-Content -Encoding utf8` write one.
- **An exported game with a large terrain has its ground** (D411). A terrain
  of 256 cells or more is saved as a folder of cells, and `ludwerk build`
  packed them where the engine never reads a cell: the game ran over nothing.
  They ship as files beside the pack now. Build the game again.
- **A game waits for the ground of a terrain saved as cells** before its
  first tick (D412), as it does for any streamed ground: a character placed on
  it no longer falls while the cells are on their way.
- **Terrain no longer shadows itself** (D368-D371, ADR 0142): its shadow is
  pushed from the light by its own slope, so a low sun no longer speckles the
  ground or facets the edge of a round shape's shadow, the far cascade no
  longer lifts a shadow two metres off what casts it, and a lamp's shadow
  pushes the ground too. Contact shadows no longer stipple ground seen at a
  slant, nor the rim of a sphere.
- **No more bright streaks at a low sun, or dark notches under a lamp, on
  flat ground** (D372): the ground's grain drew a crease wherever its noise
  stepped at a cell's edge.
- **The terrain editor does what it was asked** (terrain audit T4, D384-D391):
  Add puts no ground in the air past the ground's edge; a click on a part
  selects it rather than sculpting behind it; Apply to Voxels and Replace a
  material everywhere reach every column of a large terrain, as one step that
  keeps the redo; hills and a heightmap on a new world are one step, and a
  refusal is said; an exported heightmap imports as the same ground and a
  second export never overwrites the first; a rule's bounds stay in order;
  the brush ring and chip go when they should; Foliage's Grow is Restore.
- **A streamed terrain is drawn to its real edge** (D398, ADR 0144): ground
  past the load radius is drawn, coarse, from the cells on disk -- the same
  ground the near terrain shows -- so a world bigger than the radius no longer
  ends in a staircase of cells, nor vanishes from far off.
- **Flying high keeps the ground under you** (D397): terrain streams by how
  far a cell is across the ground, not through the air, so a camera far up no
  longer sees the ground vanish or its edge as a sawtooth -- and a flight over
  a streamed place stutters far less (its worst frames from 45 ms to 16).
- **Paint blended until it is the ground no longer shows a line of the ground
  under it** (D396) beside paint that has not reached it.
- **The ground is one surface where chunks meet** (D394, ADR 0143): a ball,
  a character or a car crossing from one terrain chunk to the next no longer
  hops, slows or jolts, and a ball rolling over any ground no longer bounces
  on the edges between its triangles.
- **Digging beside a character is five times cheaper** (D393): 10.3 ms a tick,
  worst 38.7, is 2.0, worst 3.8 (`tests/bench/terrain_dig`). A terrain moved
  by its `Position` moves its colliders rather than remeshing them.
- **A terrain edit rebuilds the navmesh tiles over it, not all of them** (D392,
  TA18).
- **A Terrain place or radius that is not a finite number is an error** (D390),
  not a silent zero, and `HeightAt` is typed `number?`.
- **No more ground floating past the edge of the loaded terrain, nor loaded
  ground left undrawn round it** (D383): a terrain node first built with
  nothing to draw was never built again when its ground streamed in.
- **The game is told the ground's painted material** (D381): a raycast and
  `ApplyRules` read the paint as the shader draws it, and the rules as it
  does.
- **Foliage casts the shadow of what is drawn** (D382): a card's holes and an
  instance fading out, not a solid rectangle.
- **A block's sides and underside draw on Vulkan -- Linux and Android**
  (D380): they drew black, the block palette past the 4 KiB a uniform block
  is bound with there.
- **Terrain layers draw their textures on Vulkan -- Linux and Android** (D379):
  the terrain's per-layer data overflowed the 4 KiB a uniform block is bound
  with there, and every layer drew flat.
- **A terrain with no materials is plain matte grey** (D374): it was tinted
  warm and cool in blotches the size of what it covered.
- **A terrain layer reads the channels its maps promise** (D375, D376): a
  metallic-roughness map's R is not occlusion unless the material says so --
  an ordinary one darkened its ground's ambient to four tenths -- the height
  comes from the material's height map, and a normal map's bumps are lit from
  the right side on every axis. Large maps no longer shimmer on the ground
  (D378).
- **A headless run that takes a picture draws the terrain** (D373): since
  terrain was built off the main thread, thirty frames run flat out were over
  before any was.
- **A streamed terrain no longer loses ground** (D366, D367): every edit, read
  and raycast loads the cells it reaches first -- all of them, or the edit is
  refused before it changes anything (past 4 096 cells); `VoxelSize` cannot
  change while cells are on disk; `Terrain:Clear()` lets go of them; a moved
  terrain streams where it is now; a cell too large to load again is not
  saved. The editor's whole-terrain actions and heightmap export read every
  cell.
- **Smooth never cuts through ground, and keeps its volume** (D365): on a thin
  slab it made a hole, and repeated passes dug a ball down below the ground it
  stood on. What comes off a bump now goes into the ground round it.
- **`WriteVoxels`, `ReadVoxels`, `WriteHeights` and `GrowBall` refuse what no
  memory holds** (D363, D364): a region whose count wrapped past 2^64, a NaN or
  huge size, a NaN occupancy, a height table steep enough to ask for gigabytes,
  and a Raise or Lower stamp larger than a Smooth's.
- **A tap and a quick double click are no longer lost** (D362): a click whose
  press and release landed between two frames never activated anything, and a
  quick double click activated once.
- **A paste right after a copy pastes** (D361): on a machine with a clipboard
  history, the paste read the clipboard while that program had it open, and
  got nothing.
- **A script that stops disconnects its connections** (D360): a stopped
  script's handlers stayed on their signals, and a game spawning scripted
  projectiles slowed without bound. `Connected` reads false after a stop.
- The script-sides audit (D344 to D359): a stopped script no longer comes back
  from a wait; a script moved in the tick a scene changes is started or
  stopped; scene changes no longer keep every closed scene's scripts in memory;
  a script cannot escape its stop by clearing `script`; `Enabled` in the editor
  starts nothing before Play; a game that is not dedicated keeps
  `global.json`'s `Client` folder; a linked stamp's overrides, the editor's
  conflict copies and its save slots no longer leak into a package; an
  attribute is never mistaken for code; a hostile server cannot grow a
  client's name table.
- The script lifecycle's defects (the script-sides ledger's S0): `.engine/trash`
  no longer ships (D331); a replica starts no script its join will destroy
  (D334); a script that survives a scene change is not started again, and a
  re-enabled script's old handlers stay stopped (D335); a hot reload comes back
  in the scene and topology the world was in (D337).
- Two flat-coloured terrain materials painted over each other meet in a
  crossfade, not a step at half cover (D329).
- **Typing reaches a `TextInput` in an exported game**, and on a phone the
  on-screen keyboard opens with it (D204).
- `TextInput.FocusLost` delivers `submitted`: true when Return left the field
  (D215). A focused, empty field shows its caret, not its placeholder (D216).
- **A client no longer fights the server over a character's speed**:
  `WalkSpeed`, `JumpSpeed`, `MaxSlopeAngle` and `AutoStepHeight` replicate, and
  a correction replays at the server's speeds (D205). Protocol version 20.
- **`.Parent` of a destroyed instance reads `nil`** rather than raising, so
  `if part.Parent then` works on any handle; every other access to a destroyed
  instance still raises `script.err.instance_dead` (D206, the owner's decision).
- **Joining the same server again from a script is the same player**, with the
  same `UserId` (D207).
- **A server that vanishes is noticed in ten seconds**, not thirty;
  `[network] timeout` in `project.toml` sets it (D208).
- **Leaving a match tells the server at once**, so the others stop seeing the
  player who left (D217).
- A headless client back in solo no longer reports an eighteen-trillion
  millisecond frame, and waits for the clock again (D209, D213).
- `Enum.KeyCode` has punctuation, Insert, PageUp, PageDown, CapsLock, NumLock
  and the keypad, appended after the existing items (D210).
- `ludwerk check` and `ludwerk fmt` skip `dist/`, and name the engine's
  definitions as the engine's (D211, D212).
- **Nothing a person made is lost or doubled by saving, moving or closing** (an
  audit of every way the editor writes, after the tester's report below):
  - The unsaved-changes dialog's **Save** saves everything unsaved -- the stamp,
    the scene, the material, a shader's tab -- *before* opening another scene or
    starting a new one, which it used to do after (losing the work); a save
    that fails stops the change. It names what it will save.
  - A `src/` script edited in its tab is written by the scene's save, a changed
    tab counts as unsaved work, and closing one keeps its text in the scene.
    A file changed outside the editor as well is never written over: the disk
    is kept and the editor's text goes to `.engine/conflicts/`.
  - **Saving during play is refused** (it saved the running game); **Save As
    never writes over another scene**.
  - Moving or renaming content rewrites only the paths that name it -- a folder
    called `terrain`, `Model` or `Tree` no longer rewrites a scene's ground key,
    a class or an instance's name.
  - A class this build does not have, and a stamp whose file is gone, are kept
    as they were written and saved back, rather than dropped for good; a
    `global.json` that could not be read is never written over or deleted.
  - **Deleting in the Content browser moves to `.engine/trash/`.**
  - A terrain's cells follow their folder; a scene renamed or duplicated takes
    its `src/scenes/<name>/` code with it; opening another material saves the
    one being edited first.
  - Nothing can be put inside the local `Player`, which a scene never saves;
    content cannot be moved during play, and a move ends the undo history
    rather than leaving steps that point at files that moved; a stamp cannot be
    pasted into itself or made from scripts that are files; a value the running
    game sets says it is not saved; text typed during play survives a
    lost-device save.
- **Scripts copied, pasted or moved between the script services no longer lose
  their code or come back twice** (reported by a tester: scripts dragged and
  pasted between client and server would not save, and were doubled after
  reopening). A copy of a script from `src/` held only a mark, so a paste
  elsewhere became an empty folder of its name, and one beside it vanished into
  the original; a drag was refused, which sent people to copy and paste. Now a
  copy carries the whole script, a drag moves it, and every save makes `src/`
  match the tree -- moving, renaming and writing files, taking the next free
  name rather than writing over one, and moving a file a script gave up to
  `.engine/trash/` instead of deleting it. A script's `Enabled`, attributes and
  tags survive a reopen too; a `ModuleScript` outside `src/shared/` is written
  as `Name.module.luau`.
- **An attribute holding a `Color3`, `CFrame`, `Vector2`, `UDim`, `UDim2` or
  `Rect` survives saving the scene.** Written untyped, a colour came back as a
  vector and the other five did not come back; the file now says the type for
  these, and reads the old spelling as before.
- **Stamps** (an audit of the whole feature): two children of one name no
  longer swap their overrides; a child renamed in one instance keeps its name
  and its edits; attributes, tags and a part's shader parameters on a linked
  instance survive a save; references to and from the children a stamp
  rebuilds survive saving the stamp; duplicating, copying and pasting a linked
  instance keeps it linked, and so does one under `GlobalScriptService`; a
  stamp at the content's root opens and places; `Instance.stamp("name")`
  finds `stamps/name.stamp.json` as documented; a stamp is never made over one
  that exists, nor inside a stamped instance; closing a stamp whose save failed
  keeps it open; a scene naming a stamp that is gone says so when it loads; and
  a stamp changed on disk during `ludwerk dev` moves its linked instances.
- **Editor defects the owner reported**: a script edited and edited back is
  not unsaved; F3 twice brings back the tabs that were in front, not the last
  one drawn; the Command Palette scrolls under the wheel; a submenu keeps its
  icon while it is open; a toast says "Undone: Edit Size" rather than
  "undid Edit"; and the default agent's settings sit under their own heading
  on `NavigationService`, where they are documented as the default agent's.
- **The editor asked to save scripts nobody had changed**: a script written
  with spaces was converted to tabs on opening and marked unsaved, and its
  scene with it -- which is every template and example. The conversion stays;
  the tabs are written with the first save of a real edit.
- **Ctrl+Shift+P, Ctrl+P, Ctrl+B, F5 and the rest did nothing with the caret
  in a script**, and what was typed next landed in the code: the code pane
  claims every key while it is active. The workbench's keys now work from any
  text field, as they do in the editor it follows; only a modal keeps them.
- **The bottom panel reopened on Stats every launch**, whichever tab had been
  left in front: the last panel to appear took the focus, and a node shows
  its focused window's tab. The Explorer takes the focus at launch, and each
  node's saved tab is put back.
- **Quick Open (Ctrl+P) could not find the code of another scene**
  (`src/scenes/<scene>/`), which is not in the world until that scene loads.
- **An orthographic camera lost what stood above it** (the owner's report,
  2026-09-27): a top-down camera standing lower than the top of a ball, a
  cylinder or a wedge showed only its shadow, and -- once depth was clipped for
  mirrors -- the tops of blocks went too. An orthographic camera now sees its
  whole column, from `FarPlane` behind where it stands to `FarPlane` in front;
  clustered lights, soft particles and the editor's picking follow.
  `ortho_gate` holds it.
- **The Export window showed an ImGui error box** ("Code uses
  SetCursorPos()/SetCursorScreenPos() to extend window/parent boundaries")
  and framed its target list in red: each target card moved the cursor to
  draw its icon and left it past the list's end. Errors of that kind now also
  reach the engine's log, once each.
- **Ctrl+Shift+S overwrote the open scene** instead of asking for a name.
- Escape now closes the Export window. A client of a dedicated server with no
  address says "no server address", instead of "joins ?".
- Two zero-length `memcpy`s from a null pointer that UBSan stops on: the UI
  gradient upload (every capture gate under the sanitizers) and an archive
  test.
- **The Android export works again.** The rename left the Gradle signing
  configuration named after the brand and its two uses named `engine`, so every
  Android export failed while evaluating `build.gradle`.
- **A dedicated server opens no graphics device.** It used to open one and draw
  every frame offscreen for nobody; on a Linux host with no GPU that device is
  Mesa's software rasteriser, and an exported server crashed in it seconds after
  starting. `--rhi=` still chooses one. **And it sleeps between ticks**: with
  no device to wait on it had spun a thousand empty frames a second on a whole
  core.
- **`ludwerk keystore new --alias upload --out release.keystore` works as its
  usage text shows.** Every option that carries a value takes `--name value` as
  well as `--name=value`; before, the space form made a keystore named `true`.
- **An exported desktop game no longer carries the editor's files** -- the
  new-project template and the editor's icon theme -- as the Android export
  already did not.
- **Messages spoke the old names**: the host's usage text (`luaug-host`, which
  now also lists `--host`, `--serve` and `--join`), `.luaug/types/`,
  `luaug.toml`, `LUAUG_TEXTURE` and `LUAUG_PARAM` in a surface shader's errors,
  `luaug build-assets`. The brand lint reads the message catalog now.
- **Home no longer carries the match controls** (player count, dedicated
  server): they are on the Test tab, whose settings Play on Home still plays.
- **A service's settings are saved with the scene when nothing is under it**,
  and a scene opened in the editor starts from the engine's settings instead of
  the last scene's (D203). A property marked `Transient` in the IDL is no longer
  written to a scene, which the IDL had said and the writer had not done:
  `Sound.TimePosition`, and now `DebugService.OverlayVisible`,
  `InputService.PointerLocked` and `PointerVisible`.

## Before Ludwerk

The engine was called **LuauG** until 2026-09-26, and released three versions under
that name. Ludwerk's own versions start again at 0.0.1 (ADR 0109): the releases
below are its history, kept as they were written.

## [2.0.0] — 2026-09-26

**A major version, because the public API broke**: a part's look is the
material it wears (ADR 0090), and `BasePart.Color` and `BasePart.Transparency`
are gone -- see *Changed -- BREAKING* for the migration. Around that change:
surface shaders, and an ocean made from one (ADR 0091); the world's look as
instances under `Lighting` (ADR 0096); voxel terrain that streams from disk;
multiplayer with prediction, interpolation, interest, remotes, teams, network
ownership, rollback and a published wire protocol, now at version 15;
navigation for several agent sizes, priced ground, links, crowds and the 2D
plane; and the 2D layer's joints, sprite animation and replication. A peer on
protocol 14 or older is refused.

### Changed -- BREAKING

- **A text's alignment is `TextXAlignment` / `TextYAlignment`** on `TextLabel`
  (and what extends it), where it was `HorizontalAlignment` /
  `VerticalAlignment`. The values are still `Enum.HorizontalAlignment` and
  `Enum.VerticalAlignment`, and a layout keeps its own `HorizontalAlignment`.
  A script writing the old names must be changed; a scene saved with them
  opens with them read under the new ones.

- **A material is an asset, and a part wears one** (ADR 0090). **This is a
  breaking change to the public API, and the release that carries it is a major
  version.**
  - `BasePart.Color` and `BasePart.Transparency` are **removed**. A part's look
    is its material's; it overrides only the parameters that material declares,
    with `SetMaterialParameter`, `GetMaterialParameter` and
    `ClearMaterialParameter`, and reads them as `BasePart.MaterialParameters`.
    The engine default material -- what a part wearing nothing wears --
    declares `Color` and `Transparency`, so `part.Color = c` becomes
    `part:SetMaterialParameter("Color", c)`.
  - The `Material` **class is removed**: `Instance.new("Material")` raises.
    A material is a `.material.json` under `content/`, with a `parent` for a
    variant and `instanceParameters` for what a part may change.
    `BasePart.Material` is a `Material?` handle.
  - `Material` is a data type: `Material.load(content)` returns the shared,
    read-only handle for an asset; `material:Clone()` returns a writable runtime
    copy. Nothing clones implicitly.
  - The scene format is version 2. A version 1 scene opens with its tints as
    overrides on the default material; `luaug migrate materials` converts a
    project's files.
  - The wire protocol is version 12: a part's material, its overrides and a
    runtime copy it wears replicate, where version 11 sent `Color` and
    `Transparency` and never the material.
  - A part's fade can no longer be tweened through `TweenService`, which moves
    properties of instances; a material parameter is neither.

### Added

- **Surface shaders** (ADR 0091): a material may name a `.surface.hlsl` the
  project writes -- two functions over the `luaug/surface.hlsli` contract, one
  that may move a vertex and one that decides what the surface is -- and every
  part wearing it is lit, shadowed and post-processed as any other.
  Parameters (`LUAUG_PARAM`) and up to five textures (`LUAUG_TEXTURE`) are
  fields of the material; a blended surface may read the scene behind it
  (`sceneDepthAt`, and `sceneColorAt` with `readsSceneColor`).
  `Material:GetShaderParameter` and `Material:SetShaderParameter` read and
  write them from a script, on a clone. The editor compiles a shader in the
  background as it is saved, writes a new one from a template, edits it with
  HLSL highlighting and its errors on their lines, and shows its parameters in
  the material panel. Building a game compiles every surface for SPIR-V, DXIL
  and MSL into the pack, and the game carries no compiler. The engine carries
  three grids a shader can displace, `luaug://mesh/grid-16`, `-64` and `-256`.
  `examples/11-ocean` draws its sea with one (6.7 ms a frame to 0.67 ms), and
  `examples/23-surfaces` is a flag, a dissolve and glass.

- **2D joints** (ADR 0102): `HingeConstraint2D` (limits and a motor),
  `SpringConstraint2D` (a soft or rigid distance) and `WeldConstraint2D`, each
  joining `Part0` and `Part1` at `Anchor0` and `Anchor1`, under the abstract
  `Constraint2D`.
- **`SpriteAnimator`** (ADR 0102): frames of a `Part2D`'s sprite sheet played on
  the simulation clock -- `FrameSize`, `Columns`, `SheetOffset`, `FirstFrame`,
  `FrameCount`, `FramesPerSecond`, `Looped`, `Playing` and the read-only
  `Frame`.
- **2D on the wire, completed** (ADR 0103, protocol 15): remote sprites are
  interpolated between snapshots, and `Tilemap2D` is replicated, its cells sent
  whole on arrival and then by changed blocks. A peer on protocol 14 is refused.

- **Sides and network ownership** (ADR 0099, protocol 14).
  - `Team` (`Color`, `AutoAssign`, `GetPlayers()`) under the new `TeamService`
    (`GetTeams()`), whose teams reach every replica. `Player.Team` is a
    player's side; a player who joins goes to the `AutoAssign` team with the
    fewest players.
  - `BasePart:SetNetworkOwner(player?)` hands a loose part to one player's
    machine, which simulates it and sends its state up; the authority follows
    it and hands it back on `nil`, when the part is anchored, or when the
    player leaves. `BasePart:GetNetworkOwner()` answers the owner.
  - The wire is protocol 14: the roster carries each player's team, channel 3
    is `Ownership`, and two messages are new. Peers of protocol 13 are refused.
- **Rollback** (ADR 0101): `RunService:SaveSimulation()` returns the 3D
  simulation as a `buffer` -- the solver's whole state, and every simulated
  part's and character's motion -- `RestoreSimulation(state)` puts it back
  (false, and nothing changed, when the simulated parts are not the same ones),
  and `StepSimulation()` steps one fixed tick without scripts or touches. A
  re-simulation from the same state with the same inputs is bit-exact.
- **The wire protocol is published** (ADR 0100): `docs/protocol/wire.md`,
  generated from the schema, states every message byte for byte, and the
  compatibility policy -- one version, matched exactly, changed whenever the
  bytes change.

- **Navigation for more than one body, over priced ground, across gaps, in
  crowds, and on the plane** (ADR 0098).
  - `NavigationService:DefineAgent(name, radius, height, maxClimb?, maxSlope?)`
    names another agent size with a mesh of its own, and `FindPath`
    takes its name as a last argument.
  - `NavigationArea`, under a part, labels the ground inside it (`Label`);
    `NavigationService:SetAreaCost(label, cost)` prices it for every agent,
    and `math.huge` forbids it. An unpriced label costs 1.
  - `NavigationLink` (`From`, `To`, `Bidirectional`, `Label`) joins two points
    the mesh does not -- a jump, a ladder. `FindPath` gains a third answer: the
    label of the link that begins at each waypoint, or `""` where the way on is
    walking.
  - `NavigationAgent`, under a part, walks it to its `Target` around every
    other agent, on the fixed tick, at `MaxSpeed`, over the mesh its
    `AgentType` names; `Reached` fires when it arrives.
  - `NavigationService:FindPath2D(from, to)` searches the plane: every
    colliding `Tilemap2D` cell and every anchored, colliding `Part2D` is a
    wall.

- **The look of a world is instances** (ADR 0096). Under `Lighting` they are
  the world's -- saved with the scene and replicated -- and under
  `Workspace.CurrentCamera` they are the viewer's own; anywhere else they do
  nothing and the editor says so. A world with none of them draws exactly as
  before.
  - `BloomEffect` (`Intensity`, `Size`, `Threshold`) governs the engine's
    bloom, and one that is disabled turns it off; `ColorCorrectionEffect`
    (`Brightness`, `Contrast`, `Saturation`, `TintColor`); `BlurEffect`
    (`Size`); `DepthOfFieldEffect` (`FocusDistance`, `InFocusRadius`,
    `NearIntensity`, `FarIntensity`); `SunRaysEffect` (`Intensity`,
    `Spread`). All five extend the abstract `PostEffect` (`Enabled`).
  - `Atmosphere` (`Density`, `Offset`, `Color`, `Decay`, `Glare`, `Haze`):
    distance and height fog lit by the time of day, meeting the sky at the
    horizon. With one, `Lighting.FogStart`, `FogEnd` and `FogColor` are kept
    and not used.
  - `Sky`: six pictures (`SkyboxBack` ... `SkyboxUp`, `SkyboxOrientation`)
    that are also what the world reflects; the sun (`SunTexture`,
    `SunAngularSize`), the moon (`MoonTexture`, `MoonAngularSize`), stars
    (`StarCount`), `CelestialBodiesShown`; and clouds (`CloudCover`,
    `CloudDensity`, `CloudColor`) drifting on `SimTime`. The sun's direction
    stays `ClockTime`'s.
  - `Lighting.EnvironmentDiffuseScale`, `EnvironmentSpecularScale`,
    `ShadowSoftness`, `GlobalShadows` and `AutoExposure`. Their defaults are
    the picture before them, to the bit; every determinism trace moved once
    for them.
  - `[graphics] depth_of_field` and `sun_rays` in `luaug.toml` (off in the Low
    preset, and depth of field in Medium), and in Project Settings.
  - The wire carries `Lighting`'s children and its five new properties:
    **protocol 13**, which a protocol 12 peer refuses.
  - `examples/22-atmosphere`, and the manual's new page, "Atmosphere, sky and
    clouds".

- **`TextLabel.TextTransparency`**: the words' own see-through, apart from the
  box's `BackgroundTransparency` (0 solid, 1 not drawn; drawn clamped).
- **The editor, from the owner's feedback pass**
  (`docs/briefs/editor-feedback-2026-09-24.md`): a ribbon across the top
  (Home, Model, Test, View); WASD flies without a held button; Ctrl+1 to
  Ctrl+4 and Ctrl+L for the tools; Alt+click selects inside a model; Shift+P
  flies free in play; Properties grouped by task with a filter that keeps its
  headings and a CFrame's rotation editable in degrees; Group as Folder; a plus
  on every instance, with every service's contents saved; a script editor with
  self-closing pairs, clickable suggestions that read the file's own code,
  type colouring, Ctrl+/, the line and word keys code editors share, and an
  error's message at the end of its line.

- **The material editor**: New Material and New Variant in the content
  browser, with a rendered ball on each material's row; a Material panel with
  its own undo that shows an edit in every world as it is made; a part's
  Properties show its material (picked, or dropped from the browser onto the
  field, an Explorer row or the part in the viewport) and each parameter the
  material lets it change.
- `assetc` compiles materials to their own pack kind, and a glTF import writes
  one material asset per material in the file.

- **Terrain debug views**: `Terrain wireframe` and `Terrain normals` in the
  editor's viewport settings, and `DebugService:ShowPanel("Terrain")` in a game.
  Triangles are drawn green, or red where one faces the wrong way, with a line
  along each vertex normal.
- **`examples/19-terrain-test`**: every controlled case of the terrain report
  side by side (flat, slope, hill, a ball added and taken away, tunnel, cave,
  wall, overhang, a chunk corner), toured from above, the side and below.

- **Terrain is a grid of voxels** (ADR 0082). Every voxel holds a material and
  an occupancy, and the surface is where the occupancy crosses one half:
  - caves, overhangs and flat ground are the same data, so nothing is special
    about any of them;
  - it is drawn as meshes with level of detail, built from each chunk's
    averages further away;
  - it collides chunk by chunk around whatever moves.

  New verbs: `FillCylinder`, `SmoothBall`, `FlattenBall`, `ReplaceMaterial`,
  `ReadVoxels`, `WriteVoxels`, `WorldToCell` and `CellCenterToWorld`. A world
  saved before this opens as it was and is saved as voxels from then on.
- `Workspace:Raycast` meets terrain anywhere, not only near things that move.
- **`TextLabel.RichText`** (F3): a label reads its text as markup -- `<b>`,
  `<i>`, `<u>`, `<s>`, `<font color size transparency>` and `<br/>`, with the
  five XML entities -- so colour, size and weight change part-way through one
  label. A tag it does not understand is drawn as text.
- **Multiplayer prediction, interpolation and interest** (ADR 0076):
  `Player.Character` names a player's part; a replica moves its own at once and
  the authority's snapshots correct it, draws everyone else between snapshots
  instead of stepping, and is sent only what is near its character. Decals and
  `Lighting` (the time of day, the light and the fog) replicate too.
- **`SurfaceGui` and `BillboardGui`** (F3): UI drawn in the world -- on a face of
  a part (`Enum.Face`, `PixelsPerMetre`) or over a point and facing the camera,
  sized in metres, in pixels, or both. The children are the screen's own
  classes, laid out the same way, and hidden by what is in front of them.
- **Terrain and block worlds stream from disk** (ADR 0075): a saved field of
  sixteen 64 m cells or more is cut into cells at play and streamed around the
  foci on the terrain radii. A cell somebody changed is never evicted, and the
  world waits for its ground on first load. `Terrain.CellSize` now names the
  grid it streams on.
- **Digging into a wall carves it**: the editor's dig aimed at steep ground, or
  with the box, takes volume out and bores forward at a steady speed while held.
- `examples/17-cave`: a tunnel into a mountain, dark inside and lit by its lamps.
- **Soft particles**: smoke and fire fade where they meet a surface instead of
  showing a hard line along it.
- **The terrain seam gate** (F1, H1): a tunnel crossing a streaming-cell
  boundary, a tile boundary and the edge between bricked and height-encoded
  ground. Every downward ray must hit the field, the collider and the drawn
  surface, and each hit must lie within a quarter-voxel of the field.
  `asset::sampleField` exposes the field's trilinear sampler, and
  `render::meshCaveColumn` exposes the cave mesh as it is drawn.
- **A dig near a hill no longer cuts it off, and a dig on a hill no longer
  deletes the ground below it** (D163): a column the brush never touched was
  written with the top or the bottom of the brush's range. This is what put
  floating plates and see-through holes in sculpted worlds.
- **Raising, lowering, smoothing and flattening work over caves** (D162): a
  column with a cave in it moves by its top and keeps the cave, where it used
  to be skipped and left a slot through the new ground. So do Generate Flat
  Ground and `WriteHeights`.
- **Digging into a selected terrain no longer draws a lid and boxes over the
  hole** (D161): the selection outline, which shows through everything, drew
  the cave meshes' buried sides. A cave draw is never outlined now.
- **A terrain larger than the atlas draws what is near the camera** (ADR
  0081). Past 16,384 tiles, a 2 km square at half a metre, the loader kept the
  first tiles in key order, and the editor drew a strip along one edge of the
  world. It now keeps the nearest, frees the far ones before uploading, and
  uploads outwards from the camera.
- **`ReplicatedStorage` and `ServerStorage`** (ADR 0080): two services that
  hold what is not the world, saved with the scene under a new optional
  `storage` key. What `ReplicatedStorage` keeps reaches every replica whatever
  its distance; `ServerStorage` stays on the authority. `scene.d.luau` types
  their contents on `game`. The Explorer takes a drop onto `Workspace` or a
  storage, which it refused before. Wire protocol 9.
- **`RemoteFunction`** (ADR 0079): a client asks with `InvokeServerAsync`,
  which yields until the authority's `OnServerInvoke` answers, and raises when
  the handler fails or there is none. The IDL gains callbacks, a function a
  script assigns and the engine calls, and the wire protocol is version 8.
  `examples/15-multiplayer` asks with H.
- **The editor's Terrain panel imports a heightmap and holds the terrain's
  settings**: a 16-bit PNG or RAW image laid over the ground at a size and
  between two heights, one undo step, and `VoxelSize`, `MinHeight` and
  `MaxHeight` beside it. The Blocks panel sets a type's images and opacity.
- **A dot reaches a child, and the scene is typed** (ADR 0078, superseding
  0061): `workspace.Player.Walker` reaches the child after the members, and
  `.luaug/types/scene.d.luau` declares the scene's tree so the analyzer types
  the path and still catches a typo. The editor writes it on open and on every
  save; `luaug setup` and `luaug-host --write-types` write it; `luaug check`
  and the starter's VS Code settings load it. Completion offers children after
  a dot.
- **`RemoteEvent`** (N2, ADR 0077): a game's own messages between machines.
  `FireServer`, `FireClient` and `FireAllClients`, received as
  `ServerReceived(player, ...)` and `ClientReceived(...)`. The authority names
  the sender from the connection. Values, instances and tables travel,
  reliably and in order. Solo and hosting, every call is delivered locally, so
  one script runs in every posture. `examples/15-multiplayer` has a horn. The
  wire protocol is version 7. A manual page, *Multiplayer*, covers the whole
  networked surface.
- **Buttons in the world are pressed** (F3): a `TextButton` on a `SurfaceGui`
  or `BillboardGui` fires `Activated` and the hover events like one on the
  screen. The pointer's ray finds it, anything solid in front hides it, and
  `AlwaysOnTop` wins. `examples/18-world-ui` has a "Next round" button on its
  scoreboard.
- **`Terrain:WriteHeights(corner, columns, heights, material?)`**: a heightmap
  in one call, one height per column, row after row. It is the verb for ground
  that comes from a generator or an image. The flagship's middle is written
  with it in under a second, where one `FillBlock` per column took 28.
- **Water flows** (V1): `VoxelService:SetBlockFluid(id, reach, ticksPerStep)`
  makes a block type a fluid. It pours down first, spreads up to seven blocks
  sideways (shallower with each one), and drains when its source is taken. It
  does not collide, `Raycast` passes through it, and `GetFluidDepth` says how
  full a block is. A world now holds up to 4,095 block types: the rest of a
  stored id is the block's state. `SetFluidReaction(from, touching, result)`
  says what one fluid becomes where it touches another: lava meeting water
  sets as stone.
- **The flagship stands on terrain** (F1, H3): the middle 512 m of
  `examples/10-open-world` is one streamed `Terrain`, with a hill and a tunnel
  through it, in place of 1,024 16-metre boxes. `tools/sculpt-ground` and
  `tools/merge_ground.luau` make and place it.
- **A replica keeps what a script holds.** An instance that leaves a replica's
  interest becomes a husk, reparented to nil, and fires
  `StreamingService.InstanceStreamedOut`, exactly as an evicted chunk's does.
  One the authority destroyed is destroyed. The wire protocol is version 6.

### Changed

- **`Lighting.Ambient` lights enclosed spaces, and the new
  `Lighting.OutdoorAmbient` lights open ones** (ADR 0084). A surface takes a
  blend of the two by how much sky it sees, so a cave is dim rather than black.
  Both start at the old `Ambient` default. A script that set `Ambient` to light
  its parts should set `OutdoorAmbient` too, since parts are outdoors.

- **The simulation is deterministic across platforms** (level C, ADR 0083). The
  same seed and the same operations give the same world hash on Windows, Linux
  and macOS. The engine ships its own `sin`, `cos`, `exp`, `log`, `pow` and the
  rest, a script's `math` and `^` use them, and no compiler may fuse a
  multiply-add the source did not write. A replay recorded on one machine plays
  back on another, and each determinism scenario has one trace for every
  platform.

- **`Terrain.VoxelSize` defaults to one metre** (was half a metre). A terrain
  that was already sculpted keeps its own.
- `Terrain.HeightAt` answers the top of the ground in a column, over a cave
  too, and between columns it blends the four around the point.
- `Terrain` brushes return how many voxels they changed, and `WriteHeights`
  counts voxels rather than columns.
- `Terrain.MinHeight` and `MaxHeight` are the world's floor and ceiling: no
  voxel is written outside them.
- `Terrain.Compact` has nothing left to do and returns 0; every edit leaves the
  voxels compact.
- `Terrain.CellCount` counts chunks of 32 voxels a side.
- **The terrain brush works on the ground as it now is, while the button is
  held.** A held Add piles up, a held raise keeps climbing and a held dig goes
  deeper, at a rate the brush's strength sets. It used to aim at the ground as
  the stroke began, and could not see its own work.
- **The terrain brush has six tools, and none of them moves a column.** Add and
  Subtract stamp a ball (or box) centred where you aim, so a click on the side
  of a cliff builds out from it rather than standing a pillar under it. The new
  Grow and Erode move the surface along its own slope (`growBall`). Smooth and
  Flatten are unchanged.
- Ground laid where there was none -- `WriteHeights`, Generate Flat Ground, a
  first raise on empty terrain -- is a slab 32 m deep rather than a column to
  `MinHeight`. A terrain's edges show walls and a bottom, all the same way.

### Fixed

- The shadow under an overhang no longer shows a row of teeth along its edge,
  and the terrain's quads are split along their shorter diagonal, so sharp rims
  no longer make long thin triangles (the owner's terrain report).
- **One ctrl+Z undoes one brush stroke** (D168). Every stroke of a session had
  joined one undo step, so a single ctrl+Z took back all of the terrain's
  edits.
- **A long sound streams instead of being decoded whole** (D129). Past ten
  seconds a file's encoded bytes are kept and each voice decodes just ahead of
  the speakers: three minutes of music costs the file's size, not about 70 MB.
  Seeking and looping work the same on a stream.
- **The tick reads a sound's length from its file's header** (D129), so a sound
  made and played on the same tick no longer decodes its whole file on the
  tick. Ogg Vorbis is included: its length is read from its last page.
- A looped sound lost one frame at every loop point, a faint click on every
  loop.
- A ball of terrain on the side of a cliff no longer stands a dark stripe down
  the wall under it. The terrain's sky term marches rays, so an overhang shades
  what is near it, not everything below it.
- **Digging caves no longer lags, and a cave no longer breaks where it meets the
  ground** (D164): terrain is one grid of voxels, so there is no join between
  two encodings to fault. A dig rebuilds only the mesh and the collider of the
  chunk it changed.
- A partitioned scene kept its block world (D157); a terrain larger than about a
  square kilometre reopened after a save (D159); terrain edges stopped hanging
  curtains to the floor as the level of detail changed.
- A tree of `Cutout` leaf blocks casts its leaves, holes and all, rather than a
  solid square.
- A script holding a streamed instance keeps a working handle when its chunk is
  evicted, and a husk nothing holds any more is destroyed (D160).

## [1.1.0] — 2026-09-23

**The editor.** v1.0.0 shipped an engine you wrote games for in a text editor;
this is the phase that gave it a window. Nine milestones, E1 through E9.

### The editor

- **`luaug edit`** — an application with a menu bar and dockable panels, a
  viewport you click, fly and select in, play/pause/step/stop/save, a content
  browser whose folders and context menus open a scene by double-clicking it,
  undo and redo, and a scene format that is what a project's world data IS
  rather than an export of it.
- **Direct manipulation**: translate, rotate and scale gizmos, creating
  instances, reparenting by drag, multi-select, `Group` and `Ungroup`, a snap
  step you can change with a grid that shows it, and a gizmo that sits where you
  say over a selection rather than where an average lands.
- **Prefabs as stamps** (ADRs 0048, 0051, 0060): `content/` holds SOURCES and an
  instance in the world may be a LINK to one. Editing a linked instance is an
  OVERRIDE and the link survives — the reverse of what ADR 0048 first said,
  reversed while the milestone was building it. A placed stamp can be asked
  which of its properties are its own, and each one reverted or applied back.
- **A launcher** (ADR 0055): `luaug-host` with no project opens a project
  browser instead of printing a usage error — recent projects, a new one from
  a template, a folder picker.
- **A script editor** (ADR 0057): any number of scripts as tabs beside the
  viewport, Luau colour from the engine's own lexer, find and replace, errors
  underlined where the parser puts them, autocomplete from the reflection
  tables, and a working debugger with breakpoints, stepping, the call stack and
  the locals — the script parked and the frame loop still drawing.
- **A look of its own** (ADR 0056): one theme as data rather than
  `StyleColorsDark` plus nine literals, Inter instead of a 13 px bitmap face,
  and a palette measured against WCAG rather than argued about.
- **A downloadable folder** (ADR 0054), and an Explorer that costs what is open
  rather than what exists.

### Assets

- **A world you author becomes a world that streams** (ADR 0053), with no
  generator script and nothing sorted by hand. `Model.StreamingMode` makes the
  model the unit rather than the part.
- **Everything arrives compiled** (ADR 0065). A loose `.gltf` is a source, not
  an asset: opening a project compiles what has no compiled form, in every host
  mode, so a clone from git works with no command. What this buys is what the
  compiler was always producing and the runtime was not reading — LOD chains,
  meshlets, and BC7 with mips instead of raw RGBA8. It is also the only reason
  an FBX has ever loaded.
- **A model becomes a `Model` of named parts**, one per primitive, each
  addressable as a URN fragment and each able to carry its own material.

### The public surface

New classes: `Attachment`, `Bone`, `Constraint`, `BallSocketConstraint`,
`HingeConstraint`, `FixedConstraint`, `Ragdoll`, `Material`, `BaseScript`,
`ModuleScript`. New enum: `AlphaMode`.

- `BasePart.Material` — a part points at a material, and `BasePart.Color`
  multiplies it, so white on both sides is the identity and no existing scene
  changes (ADR 0060). `BasePart.CanTouch`.
- `Model.Scale` — absolute rather than cumulative, about the pivot, and one
  undo takes it back. `Model.StreamingMode`.
- `MeshPart.MeshSize` — authored rather than derived, so a headless run and a
  rendered one cannot disagree about how big a mesh is.
- `PointLight.Enabled` and `SpotLight.Enabled` stop being inert and cast.
- `StreamingService` gains separate structure and terrain radii.

### Also in this release: the first of phases 2 and 4

Built while the editor phase waited for its sign-off, and shipped with it
rather than held back. Each works end to end and each has limits of its own,
listed below.

- **Terrain** (ADRs 0066, 0067, 0071): `workspace.Terrain`, one signed-distance
  field stored as height tiles where the ground is a height and as voxel bricks
  where it is not, so caves and overhangs cost only where they are. Sculpted
  from a script or the editor's brush, drawn from a height atlas on the GPU,
  collided, saved with the scene.
- **Block worlds** — `VoxelService`, which is not the terrain: registered block
  types with images per face, cutout and translucent blocks, place and break,
  a raycast to the block under the crosshair, colliders, and a block tool in
  the editor.
- **Multiplayer on a LAN** (ADRs 0069, 0070): `--host`, `--serve` and `--join`,
  snapshots diffed against acknowledged baselines, `NetworkService` and
  `Player`, and what a player did reaching the authority as intent.
- **Particles and decals** (ADR 0072): `ParticleEmitter` and `Decal`.
- **Sharper shadows and contact shadows**, and **colour textures that are
  finally drawn in the colours they were painted in** (ADR 0073): every
  compiled colour texture had been uploaded as linear and drawn pale.

New classes: `Terrain`, `VoxelService`, `NetworkService`, `Player`,
`ParticleEmitter`, `Decal`. New enums: `NetworkTopology`, `ParticleShape`,
`BlockOpacity`.

### Known limits, stated plainly

Still not built, each with an owner in the roadmap's post-v1 phases: world-space
UI (`SurfaceGui`, billboards), rich text, navmesh pathfinding, a 2D workflow and
mobile. Terrain and block worlds do not stream from disk yet; multiplayer has no
client prediction or interest management and its transport is unencrypted, so
it is for a LAN; particles are not soft; a decal darkens and tints and cannot
brighten.

**E5, E7, E8 and E9 were signed off on 2026-09-23 on the owner's delegation.**
The one row each had that only a person could close is named in its brief.

## [1.0.0] — 2026-08-22

The first release. Eleven milestones, M0 through M8, each signed off by a human
after playing what it built.

### The engine

- **A sandboxed Luau 0.734 VM** embedded directly, with `luaL_sandbox` on in
  every profile, `--!strict` under the new type solver everywhere, and the
  legacy scheduling globals (`wait`, `spawn`, `delay`, `tick`) absent rather
  than deprecated.
- **An Instance tree over a hand-rolled deterministic ECS**: `game`,
  `GetService`, `Instance.new`, `Parent`, `FindFirstChild`, attributes, tags,
  and `Clone`. A destroyed instance's handle stops resolving at the end of the
  drain in which `Destroying` fired, which is a keyed error instead of a silent
  read of a corpse.
- **Deferred-only signals** (ADR 0015) with `:Connect`, `:Once`, `:Wait` and
  `GetPropertyChangedSignal`, and `Signal<T...>` as a global datatype in place
  of `BindableEvent`.
- **A fixed 60 Hz simulation tick** with a variable render clock, five phase
  signals, and `task.spawn/defer/delay/wait/cancel`. **The renderer interpolates
  between ticks**, so a display faster than the simulation shows smooth motion.
- **Determinism as a gate, not a claim**: same build, same platform, same seed
  and inputs produce the same world hash at every checkpoint, replayed over
  10,000 ticks in CI (ADR 0025).

### Rendering

- **Forward PBR** with metallic-roughness materials, glTF 2.0 meshes, primitive
  parts, and a `Lighting` service that drives a physically-derived sky.
- **Four cascaded shadow maps** in one atlas, with a world-constant filter
  radius, normal-offset bias, a blend band rather than a switch at a plane, and
  a fit that keeps its box while the box still covers what casts.
- **Clustered forward shading** on a 16×9×24 grid: the light count stopped being
  eight.
- **Image-based lighting** by the split sum, with the sky as the environment, so
  a metal reflects the hour the script set.
- **A post chain**: depth prepass, screen-space ambient occlusion, automatic
  exposure with an artist control in EV stops, bloom, and FXAA.
- **Instanced draws**: 4,002 visible objects in 22 draw calls, against 15,390
  before.
- **A graphics settings family** (ADR 0044): `low`, `medium`, `high`, `ultra`,
  plus render scale, shadow resolution, cascade count and distance, light budget
  and post toggles — from `luaug.toml` and the command line, never from a scene.

### Physics

- **Jolt 5.6**, with rigid bodies, contacts surfaced as `Touched`, collision
  groups, raycasts and shapecasts, transform welds, and a `CharacterBody` that
  walks, jumps, climbs steps and rides moving platforms.

### The world

- **An offline asset pipeline**: assimp to canonical glTF, a content-addressed
  pack, and a build that is byte-identical across processes.
- **Chunk streaming** with a minimum ring that must be resident before a focus
  advances into it, hysteresis on eviction, and a materialisation budget in
  milliseconds rather than in chunks.
- **A per-world floating origin** (ADR 0014), verified identical at 10⁷ metres.
- **A job system** with work stealing, dependencies and a stable-merge commit.

### The game layer

- **The Input Action System** (ADR 0029): actions, contexts, bindings,
  rebindable at runtime, promptable, with a non-device channel a HUD button
  drives.
- **A UI tree** with `ScreenGui`, frames, labels, buttons, images, scrolling,
  `UDim2` layout, and text through a vendored Inter.
- **Tweens**, **audio** on the simulation timeline, and **skeletal animation**.

### Tooling

- **`luaug new`**, **`dev`** (hot reload under 500 ms, measured at 1.7 ms),
  **`run`**, **`test`**, **`check`**, **`fmt`**, **`build-assets`**, and
  **`build`** — which produces a folder you can send to somebody: the player,
  the engine's content, and the game.
- **Application identity**: a game built with `luaug build` carries its own icon
  in the artifact, verified by reading the resource back out of it.
- **A typed IDL** as the single source of truth for the public surface, from
  which the C++ registration, the `.d.luau` definitions, the api-dump and the
  API reference are all generated and freshness-gated.

### Known limits, stated plainly

Not in v1, each with an owner in the roadmap's post-v1 phases: a visual editor,
particles, decals, terrain, `SurfaceGui`, rich text, navmesh pathfinding, a 2D
workflow, multiplayer and replication, and mobile. `Sound.Content` plays a
generated tone rather than a file. `Enum.CollisionFidelity` round-trips while
every value collides as a box. Properties the engine stores and does not read
are marked `Inert` in the inspector and the api-dump, and a gate stops a new one
appearing quietly.

[2.0.0]: https://github.com/zSkanz/LuauG/releases/tag/v2.0.0
[1.1.0]: https://github.com/zSkanz/LuauG/releases/tag/v1.1.0
[1.0.0]: https://github.com/zSkanz/LuauG/releases/tag/v1.0.0
