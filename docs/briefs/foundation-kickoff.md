# The foundation to ship a game: the kickoff and the ledger

Block A of [`game-ready-plan.md`](game-ready-plan.md), approved by the owner on
2026-09-27. The order across blocks is in that file; this one is the order of
work inside block A and where each piece stands.

Decisions: [ADR 0063](../decisions/0063-https-stays-refused-and-tls-comes-from-the-platform.md)
(exists), [ADR 0111](../decisions/0111-a-game-saves-through-saveservice-into-the-players-own-folder.md),
[ADR 0112](../decisions/0112-an-exported-game-carries-bytecode-not-source.md),
[ADR 0124](../decisions/0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md) (A2b),
[ADR 0125](../decisions/0125-a-scene-is-prepared-in-the-background-and-activated-when-the-game-says.md) (A2c).

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

## Stage A1 — HTTPS through the platform's TLS (ADR 0063)

- [x] `engine/net`'s HTTP client is a seam with one implementation per
  platform (`https_winhttp.cpp`, `https_apple.mm`, `https_openssl.cpp`,
  `https_android.cpp`); the socket client stays for `http://`.
- [x] **Android**: the Java stack through JNI (`HttpURLConnection`), recorded
  below and in ADR 0063.
- [x] `https://` is accepted; certificates are verified against the operating
  system's trust store; there is no switch to disable verification.
- [x] Redirects followed (up to 5, never from `https` to `http`), in
  `performHttp` for every backend; keep-alive per host on WinHTTP,
  `NSURLSession` and Android (not on the OpenSSL path, which closes).
- [x] Tests without the public internet: a loopback TLS server with a
  certificate made at run time, refused as untrusted (Windows and Linux) and
  accepted with a root the test trusts (Linux); redirects, a POST turned GET,
  too many redirects, a downgrade refused. **Not done: the nightly job against
  a public endpoint** -- checked by hand instead (example.com, google.com with
  its redirect, and expired and self-signed certificates refused).
- [x] Docs: `@std/net`, the sandbox page and the backend guide.

## Stage A2 — `SaveService` (ADR 0111)

- [x] IDL: `SaveService`, `SaveSlot` (`Get`, `Set`, `Update`, `Remove`,
  `GetKeys`, `Save`, `Changed`, `Recovered`), `Version`, `Migrate`,
  `ListSlots`, `DeleteSlot`.
- [x] The value encoder is the scene file's; tables of values, nested; a
  function, thread, Instance or cycle is a keyed error at `Set`.
- [x] Where: `SDL_GetPrefPath(company, name)/saves/`; `.engine/saves/` in the
  editor's Play and under `engine dev`.
- [x] The file: header (magic, format version, game version, length, xxh3),
  atomic write (`.tmp`, sync, rename, previous to `.bak`), recovery from `.bak`
  with a warning, limits `[save] max_slot_bytes` and `max_slots`.
- [x] Writes on the I/O thread at most once a second, on `Save()`, on
  `BindToClose`, and on `SDL_EVENT_WILL_ENTER_BACKGROUND`.
- [x] Migration: `Migrate` runs once for an older slot before `GetSlot` returns.
- [x] Editor: a **Saves** panel (list, inspect, edit, delete, clear all).
- [x] CLI: `engine saves list|clear [--slot=]`.
- [x] Tests: round trip of every value type; a truncated file recovers from
  `.bak`; a corrupt pair gives an empty slot with `Recovered == false`; a
  migration from version 1 to 2; a slot name with `..` refused; a kill during a
  write leaves the old save intact (a test that stops the writer between rename
  steps); the Android background hook writes (the `android` stage).
- [x] An example keeps something: `examples/04-obby` remembers the last
  checkpoint and the best time.
- [x] Docs: a manual page, *Saving a game*; `api-design.md`.

## Stage A2b — a scene closes as a game does (ADR 0124)

Found by the owner on 2026-09-27: a save in a `BindToClose` handler never ran
under the editor's Stop, and reading the code showed a scene's handlers
outliving the scene.

- [x] `Scene` (a value type, not in the tree): `Name`, `Path`, `IsOpen()`,
  `BindToClose`; the global `scene` in every script's environment (a sub-world
  has its own VM, so its own scene); a closed `Scene` refuses `BindToClose` and
  `BindToMessage` by keyed error (`script.err.scene_closed`).
- [x] `SceneService.CurrentScene` is the `Scene` object; every use moved to
  `.Path` in the same commit -- `examples/24-scenes`, the `SubWorld`
  conformance scene, `engine/app/tests/world_host_tests.cpp`, the scenes guide,
  `api-design.md`, the generated reference and types.
- [x] A handler is owned by the script whose thread registered it
  (`scriptOfThread`); per-handler removal in `scenes.cpp`.
- [x] A scene closing runs its `scene:BindToClose` handlers, waits up to
  `[scene] close_grace_seconds` (default 5, simulated seconds), flushes saves,
  and drops its scripts' `game:BindToClose` handlers without running them; the
  editor, `dev` and a match's windows warn once per script
  (`script.warn.close_handler_dropped`), an exported game does not.
- [x] The game closing: the open scene's handlers, then the game's, one grace
  period, then the flush.
- [x] **The editor's Stop runs the game's close** (both lists, the grace period,
  the flush) before the restore and before the VM goes -- also when a Save or an
  Open leaves play. Not from a breakpoint: the VM is parked inside a call.
- [x] Tests (`world_host_tests.cpp`): a change waits for a yielding
  `scene:BindToClose` and lands after it; a scene script's `game:BindToClose`
  is dropped with the warning and never runs; the game's close runs the open
  scene's handlers before the game's; a global script's handler survives the
  change; the same function registered twice runs twice; a close that outlasts
  its grace is cut off with a warning and the change goes ahead. **Not
  written:** a handler registered from a scene module by a global script (the
  owner is read off the calling thread, the same path the other tests take),
  and a sub-world's close (it is `WorldHost::close`, which the game-close test
  covers).
- [x] Mailboxes (ADR 0124 §6): `game:SendMessage`/`BindToMessage` and
  `scene:SendMessage`/`BindToMessage`; deferred, in send order, each handler on
  a thread with its binder's globals (so it is the binder's, suppressed with
  it); bindings owned by the binding script; a closed scene's mailbox drops
  (editor warning); a prepared scene's (`SceneLoad.Scene`) holds until it
  opens; a topic with no handler when a message arrives drops (editor
  warning); local to the machine.
- [x] Message values (ADR 0124 §7): equal (the value types, `EnumItem`), copied
  (tables, `buffer`; a metatable is not carried and the editor warns), by
  reference (instances); functions, threads, cycles, other keys and live
  objects refused at `SendMessage` with the key path (`argument 1.a.b`). Each
  handler gets its own copy.
- [x] Tests (mailboxes): scene → game and game → scene; a scene script's
  binding does not fire after its scene closes; a message to `loading.Scene`
  arrives after its scripts start and before `SceneLoaded`; a received table
  changed by the receiver leaves the sender's intact; a function in a nested
  table is refused naming its path. A message on a server never reaching a
  client needs no test: nothing of it touches the wire.
- [x] Docs: `Scene` in the reference (generated); the scenes guide (*The
  scene*, *When a scene closes*, *Messages between the game and a scene*) and
  the saves guide (*Saving at a close*); `api-design.md`.
- [x] Open for the owner (ADR 0124): whether `BindToClose` returns a
  `Connection`. **Built without it** -- it returns nothing, as before -- and
  per-handler removal exists inside, so exposing it later is a small change.
  `BindToMessage` returns nothing either, for the same reason.

## Stage A2c — a scene prepared in the background (ADR 0125), built with A2b

The owner asked for this to be built together with A2b: the activation is
where the old scene's `scene:BindToClose` runs.

- [x] IDL: `SceneService:LoadSceneAsync(path, options?)` → `SceneLoad` (`Scene`,
  `Path`, `Progress`, `Status`, `Error`, `Ready`, `Finished`, `Activate`,
  `Cancel`); `Enum.SceneLoadStatus` appended at the end of `enums.api.luau`.
  **`LoadScene` keeps its own path** rather than becoming `LoadSceneAsync` with
  `Activate = true`: it behaves exactly as before (the change at the same tick),
  which is what ADR 0125 promised of it, and it cancels a load in flight.
- [x] The scene reader gains a detached target: `scene::parseScene` reads and
  checks a file into a `ParsedScene` on a job; `readScene(world, parsed)`
  instantiates it on the main thread.
- [~] Assets warmed from the parse: **meshes and pictures** (every
  `asset://*.gltf|glb|png|jpg|jpeg|ktx2` the parse names, through the mesh
  loader's own feeds, `MeshLoader::warmMeshes` and `warmTextures`, which F8's
  preloading shares). **Not yet:** materials, sounds, and terrain and block
  cells within `[scene] warm_radius`; `[scene] max_prepared_bytes`.
- [x] `Progress` measured and monotonic: half at the parse, the rest the share
  of the names it holds that have arrived (loaded or given up on).
- [x] `Activate()` at the next safe point: `SceneLoading`, the old scene's
  `scene:BindToClose` waited for (A2b), saves flushed, teardown, instantiation
  from the prepared parse, scripts started, held messages, `SceneLoaded`,
  `Finished(true)`.
- [x] One load at a time (a second, or a `LoadScene`, cancels the first,
  logged); `Cancel()` drops it and `Finished(false)` fires.
- [~] In a match: authority only (a client's call is a keyed error). **Not yet:**
  clients preparing when the authority starts one -- they load at the switch,
  as they do for `LoadScene`, which needs a wire message of its own. A
  `SubWorld` runs its own host, so the same call loads its next scene.
- [ ] F3 and Stats: a prepared scene, its status and bytes.
- [x] `examples/24-scenes` uses it: the menu asks the game by message, the game
  prepares the arena behind a progress bar, fades, `Activate()`; the arena
  prints from `scene:BindToClose`.
- [x] Tests: the current scene keeps ticking while another prepares; `Progress`
  reaches 1 and `Ready` fires; `Activate` switches with the data; `Cancel`
  frees it and a later `Activate` raises; a second call cancels the first; a
  missing scene fails with `Error`; `LoadScene` behaves as before (its tests
  unchanged). **Not written:** "the switch loads no asset synchronously" (the
  mesh loader has no synchronous path left to catch) and a client activating on
  the authority's tick (not built).
- [x] Docs: the scenes guide's *Loading in the background*; the reference;
  `api-design.md`; ADR 0106's amendment note.

## Stage A3 — bytecode in the export (ADR 0112)

- [x] `engine build` compiles every script with the runtime's compiler options
  (O2, debug level 1) and packs bytecode; no `.luau` in the package.
- [x] The player loads bytecode and refuses an out-of-range version by keyed
  error.
- [x] `[export] ship_source = true` keeps source, shown in the Export window.
- [x] Tests: the sentinel string of a script's source is absent from every
  target's package; an error in an exported game names script and line.
- [x] Docs: the shipping guide; ADR 0045's amendment note.
- Compiled by the host itself (`engine-host --compile-scripts <game>`), so
  the compiler and its options are the engine's; a script is `init.luauc`
  beside where its source was, mounted and required by the source's name.

## Findings

- **`scene.Name` was read once, at load.** The compiler turns `global.field`
  into an import, and a safe environment resolves an import when the chunk
  loads and keeps the answer: every script's `scene.Name` said the scene it was
  loaded in, for ever. `scene` is in the compiler's `mutableGlobals` now
  (`compile_options.h`), which makes its fields read when the code runs. The
  same is true of `game.X` and `workspace.X`, which is harmless only because
  what they name does not change under a running script.
- **`IsOpen` is a method, not a property**: §9 gives booleans with an `Is`
  prefix to methods (`IsSubWorld()`), and the checker refuses the property.
- **`LoadSceneAsync` does not yield.** §9 reserves `Async` for a method that
  parks its caller; the owner's name is kept, and the schema gained
  `ReturnsHandle` for the other honest meaning -- the call returns at once with
  a handle to work that finishes later.
- **The close's grace is simulated time, not wall clock.** The tick a scene
  change lands on is part of what a replay reproduces (R10); the game's own
  close keeps its wall-clock cap, because nothing is replayed after it.
- **A headless run prepares at the safe point, whole.** On a job, the tick
  `Ready` fires on would depend on the machine; a replay, a conformance run and
  a dedicated server prepare inline and are ready on the tick they asked.
- **A replica follows its authority at once**, so its scene's handlers get one
  pass and are not waited for: the authority's world has already moved on.
- **A handler's thread carries its binder's globals**, not the sender's: a
  message handler is suppressed with the script that bound it, and `script`
  inside it is that script.

- **Linux's OpenSSL is loaded, not linked**: a binary linked against one soname
  would not start on a machine with the other, and loading it at the first
  `https://` request needs no OpenSSL headers to build either -- a dozen
  functions declared with opaque pointers.
- **A test's certificate cannot be committed**: a private key in a public
  repository is found and reported. Each run makes its own -- Schannel on
  Windows needs the key in the user's store under a name (an ephemeral key is
  in the wrong process for it), deleted after.
- **The conformance suite cannot make a network request**: it runs unthrottled,
  so a request that waits even a second on the network outlives the suite's
  simulated time budget, and the suite never reports. Its https case is a URL
  refused while it is read, for a reason other than being https.
- **The bytecode version is the tab.** Luau 0.734 writes version 9, which is
  the byte `0x09`, and reads up to 13 -- so "bytecode begins with a byte below the tab"
  was wrong; the second byte (the types version, below four) is what no text
  has. The refusal range is what the VM reads (`LBC_VERSION_MAX`), not what the
  compiler writes (`TARGET`), as `luau_load` itself does.
- **The local packaging test ran a stale player.** The `windows` stage packages
  with whatever `win-msvc-player` exists, and only the opt-in `winprofiles`
  stage rebuilds it -- a new host flag fails there until the player is built.
