# 0265 — A game opens from a link: two web builds, and a loop that yields

- Status: accepted. The web is open (R15, 2026-10-10), and its dependencies
  were approved by the project's maintainer on 2026-10-26 (R5, R6): the
  Emscripten SDK (MIT/UIUC), Dawn's emdawnwebgpu bindings (BSD-3), Tint
  (Apache-2.0, offline only, never in a game) and, later, msquic (MIT, server
  side only). Each at a pinned release; none vendored by this ADR.
- Date: 2026-10-26
- Builds on: ADR 0023 (the RHI is a seam, and a backend is a build-time
  choice), ADR 0104 (the export window and its targets)

## The question

A game made with the engine should open from a link: a page on itch.io or on
the developer's own site, no install. That needs the engine compiled to
WebAssembly, and four questions answered before any of it is built:

1. Which toolchain.
2. How the game's frame loop -- one function that runs until the game ends --
   gives the browser its thread back once a frame.
3. Threads: a browser has them only on a cross-origin-isolated page, and
   many pages a game is played on are not.
4. Whether the engine fits: in size, and in what its main thread may do,
   since the browser's main thread may not block.

Answered by a spike before this ADR was written: the engine as it is, built
with Emscripten 6.0.13 into a host that runs under Node, headless, on the
null graphics device. The numbers below are that spike's.

## What was measured

**It builds and runs.** The host, Luau, the job pool, Jolt, Box2D, Recast and
replication compile to wasm32 with about thirty small fixes, all of one kind
(an unsigned index added to an iterator, which is a sign conversion where
`long` is 32 bits). `examples/03-physics-playground` streams its cells and
runs; `--bench` runs every benchmark.

**Size** of the `.wasm`, debug names stripped, with no graphics backend yet:

| Build | Raw | gzip | brotli |
|---|---|---|---|
| `player`, threads | 15.45 MiB | 5.52 MiB | 3.94 MiB |
| `player`, threads, Asyncify on the loop | 15.54 MiB | 5.56 MiB | 3.97 MiB |
| `dev`, threads | 15.67 MiB | 5.58 MiB | 3.98 MiB |
| `dev`, one thread | 15.50 MiB | 5.44 MiB | 3.92 MiB |

The JavaScript beside it is 249 KB. The budget is 20 MiB raw and 6 MiB
brotli, so about 2 MiB of brotli is left for the WebGPU backend.

**A frame's yield through Asyncify**, limited to the five functions between
`main` and the frame loop (`main`, `engineHostMain`, `hostMain`,
`engine::app::run`, `runSession`): one yield at the top of every frame,
awaiting a promise that is already resolved. So what was measured is the
stack's unwind and rewind, and no timer. Over 3,000 frames of the physics
playground, three rounds alternated:

- without: 764, 776 and 790 ms;
- with: 1,300, 1,167 and 1,201 ms.

That is **0.15 ms a frame**, under 1% of a 60 Hz frame. The code grows by
95 KB, and the link takes 225 s instead of 63 s.

**CPU**, per tick, the same machine, native Linux `player` against wasm:

| Benchmark | Native | wasm, threads | wasm, one thread |
|---|---|---|---|
| churn10k | 5.90 ms | 9.53 ms | 8.79 ms |
| physics1k | 1.04 ms | 1.61 ms | 2.35 ms |
| physics1k, the physics step alone | 0.87 ms | 1.33 ms | 2.17 ms |
| instances500 | 0.62 ms | 1.01 ms | 0.68 ms |
| terrain_dig | 2.23 ms | 3.41 ms | 5.03 ms |
| ragdoll10 | 0.40 ms | 0.53 ms | 0.35 ms |

wasm costs about one and a half times native. One thread costs more only
where the work runs in parallel: Jolt's solver and the terrain's jobs, about
1.6 times. A light scene is faster on one thread, because nothing is woken
and there are no atomics.

## Decision

### 1. Emscripten, pinned

The Emscripten SDK at a pinned release (6.0.13 to start), installed in the
Tier-2 image and by `scripts/install-web.ps1`. SDL is built with
`SDL_PTHREADS=ON` in the threaded build: it defaults to off on Emscripten,
and without it SDL has no asynchronous file reading. wasi-sdk with a JS host
of our own was the alternative. It was rejected: no SDL, no threads story,
and everything Emscripten's ports give would be written here.

### 2. The loop yields through Asyncify, limited to its own stack

The frame loop stays one function. Once a frame it yields to the browser,
which resumes it on the next animation frame. `ASYNCIFY_ONLY` names the
functions on the loop's stack and nothing else, so no other code pays for
it. The two alternatives were rejected:

- **The loop rewritten as a `step()` per frame.** It is the largest change in
  the engine, and the measurement does not need it.
- **The game on a worker (`PROXY_TO_PTHREAD`)**, where blocking is allowed.
  It cannot exist in a build without threads, which this ADR also makes
  (§3). And the waits it would make legal have to go anyway (§4).

### 3. Two web builds, both supported

- **`web-threads`**: pthreads on `SharedArrayBuffer`, for a page that is
  cross-origin isolated -- a developer's own site with the two headers, or
  itch.io with its "SharedArrayBuffer support" option on.
- **`web-single`**: no threads at all, for every other page. It is the
  default for itch.io, so the game also opens embedded and in a browser
  that cannot isolate the page there.

The export picks the build by target, and a flag overrides it. The one-thread
build is real code, not a build with threads switched off. What starts a
thread today goes behind **a small worker abstraction in `platform`**, with a
synchronous implementation: work handed to a worker runs on the caller's
thread, at the point where the caller would have waited for it. That
covers:

- the job pool: queues with no threads, which `wait` drains itself;
- the asynchronous file service: on one thread every caller reads the file
  itself, the contract `readFileAsync` already states;
- the save store and the crypto service;
- the network's own threads, which the web replaces with a WebSocket anyway.

Jolt already runs on its caller's thread when it is built without threads.

In the threaded build, the pthread pool is sized at start-up from
`navigator.hardwareConcurrency`, with a cap. The engine's own pool takes one
fewer than the machine has, and Jolt takes four per world. A thread the pool
cannot supply starts only after the next yield. Nothing deadlocks on that:
both the job pool's `wait` and Jolt's barrier run work on the thread that
waits. But such a thread joins a frame late, so the pool is sized to avoid
it.

### 4. Nothing on the main thread blocks

The browser's main thread may not block. Where Emscripten lets a wait through
there, it spins, and it hangs if the wait depends on the main thread itself.
Every wait on the frame's thread was inventoried. Two have to change, and
they change **on every platform**, so that a native build runs the same
code:

- **`waitIdle` leaves the frame path.** It is called at eight places in the
  frame loop: resizing, losing the device, the end. WebGPU has no blocking
  wait. Where the engine needs the GPU idle, it asks for a signal when the
  submitted work is done, and goes on in a later frame.
- **`IDevice::readTexture` becomes asynchronous.** A screenshot or a picture
  read back returns a handle that is ready in a later frame, because WebGPU
  only maps a buffer asynchronously.

The rest stay as they are, for the reasons given:

- the job pool's `wait` and Jolt's barriers run work while they wait;
- the save store's flush happens on a worker in the threaded build and
  inline in the single one;
- the clipboard's poll and a headless server's 1 ms sleep are not on a web
  player's path.

### 5. Sizes are measured on every change to the web player

The table above is the first row of a size table kept in the web manual, as
ADR 0260's sizes were: raw and brotli, both builds, for each change that
moves them. The budget stays 20 MiB raw and 6 MiB brotli for the `.wasm`.

## itch.io

What itch.io's documentation states for an HTML game:

- at most 1,000 files, 200 MB per file and 500 MB unpacked, with paths of at
  most 240 characters;
- `.wasm` and `.js` are gzipped by its CDN, and a `.br` file is served as
  brotli;
- the "SharedArrayBuffer support" option sends `Cross-Origin-Opener-Policy`
  and `Cross-Origin-Embedder-Policy`, and moves the game to another domain,
  `html.itch.zone`. Embeds on other sites stop working, and Safari opens the
  game in a pop-up.

**Turning the option on, or off, loses a player's saves.** itch.io's own note
says this of `localStorage`. The web player keeps its saves in IndexedDB,
which belongs to an origin just as `localStorage` does, so the move to
another domain leaves those saves behind as well. That follows from the
browser's same-origin rule; it has not been tried on itch.io itself. The
manual says it where it says how to export, and so does the export window
beside the option: choose before the first player saves.

## Consequences

- The wasm32 fixes the spike needed go into the engine now. So does one
  defect it found on every platform: a streamed cell whose read could not
  start asynchronously was lost instead of read.
- The renderer gains an asynchronous way to wait for the GPU and to read a
  texture back. Both are used on native too.
- `platform` gains the worker abstraction. Every thread the engine starts
  goes through it, and a test runs each user on the synchronous
  implementation.
- Two web players ship in the installation, and the exporter chooses
  between them.
- Not decided here, and each the subject of its own ADR:
  - storage and assets (IndexedDB, the pack over HTTP), 0266;
  - the WebGPU backend and WGSL, 0267;
  - networking over WebSocket and the relay's TLS, 0268;
  - the export and its shell, 0269.
