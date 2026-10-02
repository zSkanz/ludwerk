# The game-ready plan: the kickoff and the order of four ledgers

On 2026-09-27 the owner asked what the engine still lacks to be one a person
ships a game with, and approved the answer as a plan, in full, the same day.
The engine had the technology; what it lacked was what a released game needs —
keeping a save, reaching the internet safely, not handing its code to whoever
opens the folder — plus a terrain that paints with real materials, foliage and
wind, water that floats a boat by itself, and primitives that let an author make
what the engine did not foresee.

**Approved by the owner on 2026-09-27**, in conversation:

- the whole plan, blocks A to D, in the order below, and block E (parallel
  scripts) and block F (the toolkit: click, prompt and drag, movers and
  constraints, UI, highlight/beam/trail, solids, sound effects, vibration,
  chat, preloading) added the same day at the owner's word;
- **its place in the queue: after Views ([`views-kickoff.md`](views-kickoff.md),
  ADR 0107) and before the AI panel ([`ai-kickoff.md`](ai-kickoff.md),
  ADR 0108)**;
- the three new dependencies (R5): **libsodium** (ADR 0120), **dav1d** and
  **libopus** (ADR 0122).

This file is the order across the six ledgers. Each ledger is the order of work
inside its block and where each piece stands; each ADR is the decision. **Read
the ADR before its stage.**

| Block | Ledger | ADRs |
|---|---|---|
| A — the foundation to ship a game | [`foundation-kickoff.md`](foundation-kickoff.md) | 0063 (exists), [0111](../decisions/0111-a-game-saves-through-saveservice-into-the-players-own-folder.md), [0112](../decisions/0112-an-exported-game-carries-bytecode-not-source.md) |
| B — terrain, foliage, wind and water | [`world-kickoff.md`](world-kickoff.md) | [0113](../decisions/0113-terrain-layers-are-engine-materials-and-rules-paint-by-slope-and-height.md), [0114](../decisions/0114-a-voxel-holds-two-materials-and-a-blend.md), [0115](../decisions/0115-wind-is-a-workspace-property-that-moves-what-is-drawn.md), [0116](../decisions/0116-foliage-is-drawn-from-rules-over-terrain-and-never-simulated.md), [0117](../decisions/0117-a-material-carries-friction-and-a-footstep-sound.md), [0118](../decisions/0118-water-is-one-wave-definition-read-by-the-renderer-and-by-physics.md) |
| A (added) | [`foundation-kickoff.md`](foundation-kickoff.md) | [0124](../decisions/0124-a-scene-closes-as-a-game-does-and-a-handler-dies-with-its-script.md), [0125](../decisions/0125-a-scene-is-prepared-in-the-background-and-activated-when-the-game-says.md) |
| C — the network | [`network-kickoff.md`](network-kickoff.md) | [0119](../decisions/0119-networkservice-opens-connections-and-a-project-declares-what-it-may-do.md), [0120](../decisions/0120-the-transport-is-encrypted-and-a-player-is-a-key.md) |
| D — creativity and media | [`media-kickoff.md`](media-kickoff.md) | [0121](../decisions/0121-a-script-can-write-an-image-a-sound-and-a-mesh.md), [0122](../decisions/0122-a-videoplayer-decodes-av1-and-opus-and-the-platforms-h264.md) |
| E — parallel scripts | [`parallel-kickoff.md`](parallel-kickoff.md) | [0123](../decisions/0123-scripts-run-in-parallel-inside-actors-and-commit-in-a-fixed-order.md) |
| F — the toolkit | [`toolkit-kickoff.md`](toolkit-kickoff.md) | [0126](../decisions/0126-a-part-can-be-clicked-prompted-and-dragged-without-code.md), [0127](../decisions/0127-movers-and-constraints-move-a-part-without-code.md), [0128](../decisions/0128-ui-lays-out-in-grids-and-pages-adapts-to-any-screen-and-is-driven-by-a-gamepad.md), [0129](../decisions/0129-highlight-beam-and-trail.md), [0130](../decisions/0130-parts-combine-into-solids-and-two-more-shapes.md), [0131](../decisions/0131-sound-effects-vibration-and-preloading.md), [0132](../decisions/0132-players-chat-through-textchatservice.md) |

## The order

Legend: `[ ]` not started · `[~]` in progress · `[x]` done.

| # | Stage | Ledger | Why here |
|---|---|---|---|
| [x] 1 | **A1** HTTPS through the platform's TLS · **A2** `SaveService` | foundation | A game that saves and talks to the internet |
| [x] 2 | **B1** terrain layers are materials · **B2** rules by slope and height | world | The base of everything the terrain does next |
| [~] 2b | **A2b** a scene closes as a game does, and `game` and `scene` are mailboxes (ADR 0124) · **A2c** a scene prepared in the background (ADR 0125), built together | foundation | Found in use on 2026-09-27: a close handler never ran under Stop; loading screens freeze |
| [x] 2c | **F8** `ContentProvider:PreloadAsync` | toolkit | Shares A2c's warming path |
| [x] 2d | **F1** `ClickDetector`, `ProximityPrompt`, `DragDetector` | toolkit | The owner: "certain we need it" |
| [x] 3 | **B5** wind | world | Small, and foliage needs it |
| [~] 4 | **B6** foliage | world | The largest visual gain |
| [x] 5 | **B3** two materials a voxel, paint modes, the seam · **B4** terrain tools | world | Painting as a person expects it |
| [x] 6 | **B8** water, and the ocean example rewritten | world | Fixes an example that exists; any game with a boat |
| [ ] 6b | **F2** movers and constraints (motor, servo, align, rope, rod, spring) | toolkit | Builds on B8's impulses at a point; a part moves without code |
| [ ] 7 | **C1** `Request` and WebSocket in `NetworkService` | network | What people ask of a network first |
| [ ] 7b | **F3** UI grids, pages, flex, scale, size constraints, `CanvasGroup`, `UIDragDetector`, gamepad selection | toolkit | UI for every screen and a controller |
| [x] 7c | **F4** `Highlight`, `Beam`, `Trail` | toolkit | — |
| [x] 8 | **A3** bytecode in the export | foundation | The shipped game stops carrying its source |
| [x] 9 | **B7** friction and footsteps | world | — |
| [ ] 9b | **F5** solids (union, subtract, intersect) and two shapes | toolkit | Manifold adopted (2026-09-30); built after the block-world stages |
| [x] 9c | **F6** sound effects and vibration | toolkit | — |
| [ ] 10 | **E0 to E4** actors: scripts in parallel, committed in a fixed order | parallel | A game's own logic on every core |
| [ ] 11 | **D1** `EditableImage` and `AudioStream` · **D3** `EditableMesh` | media | Creativity |
| [ ] 12 | **C4** encryption, a player is a key, relay | network | Multiplayer across the internet |
| [ ] 12b | **F7** text chat | toolkit | After encryption and a player's identity |
| [ ] 13 | **D2** `VideoPlayer` | media | Builds on D1 and C1 |
| [ ] 14 | **C2** TCP, UDP and listening · **C3** the Network panel | network | The rarer cases |

## What must hold at every stage

- `scripts/localgate.ps1` green on every stage, Linux and Android included,
  before every push; then CI read. Never write to a red `main`.
- **A game that uses none of it behaves exactly as before**: the reference
  screenshots (ADR 0038) and the determinism traces do not move, unless the
  stage's ADR says which one moves and why (ADR 0114 and 0117 each re-record
  once).
- Frame-time and memory before and after in `docs/perf-baselines.md` for every
  stage that touches the renderer, the terrain or the physics step.
- New enums are appended at the END of `api/defs/enums.api.luau`; after any IDL
  change run `gen_cpp`, `gen_dts`, `gen_dump`, `gen_reference`.
- R3: every new message is an i18n key. R7: nothing in code names another
  engine. R17: no backend type in the public API. R10 for everything that
  enters the simulation.
- Each stage ends with its manual page, its CHANGELOG entry, and its line here
  ticked.

## Not in this plan (the backlog it leaves)

A complete game made with the engine and published; Web, iOS and consoles;
animation state machines, blending and IK; global illumination and TAA or
upscaling; foliage a character pushes aside.
