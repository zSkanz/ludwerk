# Roadmap

This page lists what is planned for Ludwerk, in the order it is expected to be
built. It is a statement of intent, not a schedule. When an item is built, it
moves to [Built](#built) and names the decision that records it.

## Next

- **Editor**: more timeline tools, faster exports that reuse the import cache,
  and assets that stay in use while a new version is being saved. The in-place
  asset swap is designed in [`docs/api-design.md`](docs/api-design.md) and not
  built yet.

## Later

- **Web**: games that run in a browser from a link. The decision is
  [ADR 0265](docs/decisions/0265-a-game-opens-from-a-link.md); its build
  presets and install script are not in the tree yet.
- **Packages**: a package manager for sharing Luau modules and assets.
- **Animation**: root motion, and merging a character's worn pieces into one
  mesh. Clips play in place today ([ADR 0197](docs/decisions/0197-animation-graphs.md)).
- **Profiler**: a flame chart over time and a live connection to a game
  running on another device. The frame report is text only.
- **Node editor**: sub-graphs and comments.

## Built

- **Physics**: vehicles on cast wheels with suspension
  ([ADR 0213](docs/decisions/0213-a-vehicle-is-a-chassis-on-cast-wheels.md)),
  cloth simulated as a mesh
  ([ADR 0214](docs/decisions/0214-cloth-is-a-mesh-on-soft-bodies-still-picture.md)),
  and destruction by cutting a part into pieces held by breakable joints
  ([ADR 0215](docs/decisions/0215-destruction-is-a-part-cut-into-pieces-held-by-joints-that-break.md)).
- **Audio**: a sound behind a wall is muffled
  ([ADR 0255](docs/decisions/0255-a-sound-behind-a-wall-is-muffled.md)), and
  players talk to each other with Opus voice chat
  ([ADR 0256](docs/decisions/0256-players-talk-to-each-other.md)).
- **World**: automatic levels of detail
  ([ADR 0261](docs/decisions/0261-a-mesh-changes-its-level-without-being-seen-to.md)),
  occlusion culling
  ([ADR 0262](docs/decisions/0262-what-a-wall-hides-is-not-drawn.md)), and a
  texture streaming budget with a memory budget for what a world keeps
  ([ADR 0264](docs/decisions/0264-what-the-world-no-longer-needs-is-let-go.md)).

## Not planned yet

- iOS and console platforms other than Xbox in developer mode.
- Hardware ray tracing.
- Virtual reality.

To propose something that is not on this page, open an issue.
