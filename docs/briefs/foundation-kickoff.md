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

- [ ] `Scene` (not creatable, not in the tree): `Name`, `Path`, `IsOpen`,
  `BindToClose`; the global `scene` in every script's environment (and in a
  `SubWorld`, the sub-world's scene); a closed `Scene` refuses `BindToClose` by
  keyed error.
- [ ] `SceneService.CurrentScene` becomes the `Scene` object; every use moves to
  `.Path` in the same commit -- `examples/24-scenes/src/client/Loading.luau`,
  `engine/app/tests/world_host_tests.cpp`, `docs/manual/guides/scenes.md`,
  `docs/api-design.md`, the generated reference and types.
- [ ] A handler is owned by the script whose thread registered it (not the
  module its code lives in); per-handler removal in `services.cpp`.
- [ ] A scene closing runs its `scene:BindToClose` handlers, waits up to
  `[scene] close_grace_seconds` (default 5), flushes saves, and drops its
  scripts' `game:BindToClose` handlers without running them; the editor warns
  once per script (`script.warn.close_handler_dropped`), an exported game does
  not.
- [ ] The game closing: the open scene's handlers, then the game's, one grace
  period, then the flush.
- [ ] **The editor's Stop runs the game's close** (both lists, the grace period,
  the flush) before the VM goes.
- [ ] Tests: game close runs scene then game handlers in that order; Stop runs
  them; `LoadScene` runs the old scene's `scene:BindToClose`, waits for a
  yielding `SaveAsync`, and drops its `game:BindToClose`; a global script's
  handler survives a scene change; a handler registered from a scene module by a
  global script is the global script's; a sub-world's close runs its scene's
  handlers; the same function registered twice runs twice.
- [ ] Mailboxes (ADR 0124 §6): `game:SendMessage`/`BindToMessage` and
  `scene:SendMessage`/`BindToMessage`; deferred, in send order; bindings owned
  by the binding script; a closed scene's mailbox drops (editor warning); a
  prepared scene's (`SceneLoad.Scene`, A2c) queues until it opens; a topic with
  no handler drops (editor warning); local to the machine.
- [ ] Message values (ADR 0124 §7): equal, copied (tables, `buffer`; metatables
  not carried, editor warning), by reference (instances); functions, threads,
  cycles and live objects refused at `SendMessage` with the key path.
- [ ] Tests (mailboxes): scene → game and game → scene round trips; a scene
  script's binding does not fire after its scene closes; a message to
  `loading.Scene` arrives before `SceneLoaded`; a received table changed by the
  receiver leaves the sender's intact; a function in a nested table is refused
  naming its path; a message on a server never reaches a client.
- [ ] Docs: `scene` and `Scene` in the reference; the saves guide and the scenes
  guide say which close to use for what; `api-design.md`.
- [ ] Open for the owner (ADR 0124): whether `BindToClose` returns a
  `Connection`.

## Stage A2c — a scene prepared in the background (ADR 0125), built with A2b

The owner asked for this to be built together with A2b: the activation is
where the old scene's `scene:BindToClose` runs.

- [ ] IDL: `SceneService:LoadSceneAsync(path, options?)` → `SceneLoad` (`Path`,
  `Progress`, `Status`, `Error`, `Ready`, `Finished`, `Activate`, `Cancel`);
  `Enum.SceneLoadStatus` appended at the end of `enums.api.luau`;
  `LoadScene` becomes `LoadSceneAsync` with `Activate = true`.
- [ ] The scene reader gains a detached target: read and parse on a job into a
  representation the main thread instantiates later.
- [ ] Assets warmed from the parse (meshes, textures, materials, sounds; terrain
  and block cells within `[scene] warm_radius` of the spawn), held by the handle;
  `[scene] max_prepared_bytes` (lower default on Android).
- [ ] `Progress` from bytes parsed and bytes warmed, monotonic.
- [ ] `Activate()` at the next safe point: `SceneLoading`, the old scene's
  `scene:BindToClose` waited for (A2b), saves flushed, teardown, instantiation
  from the prepared scene, scripts started, `SceneLoaded`.
- [ ] One load at a time (a second cancels the first, logged); `Cancel()` frees
  what was warmed.
- [ ] In a match: authority only; clients prepare when it starts and activate at
  the same tick, waiting if not ready. In a `SubWorld`: its own next scene.
- [ ] F3 and Stats: a prepared scene, its status and bytes.
- [ ] `examples/24-scenes` uses it: a progress bar, a fade, `Activate()`.
- [ ] Tests: the current scene keeps ticking while another prepares (its trace
  unchanged until `Activate`); `Progress` rises to 1 and `Ready` fires; the
  switch after `Ready` loads no asset synchronously; `Cancel` frees the bytes;
  a second call cancels the first; a client in a match activates on the
  authority's tick; `LoadScene` behaves as before.
- [ ] Docs: the scenes guide's loading-screen section; the reference;
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
