# What is not here

Written so you find out now rather than three weeks in.

Everything below is genuinely absent. Where something is *planned*, it says so;
where it is not, it says that too.

## The platform

| Missing | State |
|---|---|
| Accounts, a players service, friends, avatars | **Not planned.** This is an engine, not a hosted platform. |
| Data stores | **Not planned.** Persistence is a backend you write. |
| Marketplace, monetization, analytics | **Not planned.** |
| Matchmaking and hosted servers | **Not planned.** A match is hosted by a player's machine (`NetworkService:Host()`, or `--host`) or a server you run (`--serve`), and a script joins one with `NetworkService:Join(address)`; finding one is your backend's job. |

Replication and a game's own messages are here: see
[Multiplayer](manual:guides/multiplayer). Anything that outlives a match is one
HTTP client and your own server: see [Talking to a backend](manual:guides/backend).

## Rendering and world content

| Missing | State |
|---|---|
| Complex-script shaping | **Not scheduled.** A label lays its codepoints out left to right, so Arabic, Devanagari and Thai do not join. Rich text is here: `TextLabel.RichText`. |
| An HDR panorama sky | **Not present.** A `Sky` takes six images, and reflections come from it; an HDR panorama is not a format it reads. See [Atmosphere and the sky](manual:rendering/atmosphere-and-sky). |
| Screen-space reflections | **Not scheduled.** What ships is image-based lighting from that sky. |
| Temporal anti-aliasing, upscalers, frame generation | **Not present**, and blocked on a velocity buffer that does not exist. |
| Motion blur, film grain, lens flare, a vignette | **Not present.** Depth of field, colour correction, bloom, blur and sun rays are here, as effects under `Lighting` (see [The post chain](manual:rendering/post)). |
| A camera drawing into a texture (monitors, mirrors, portals), a `ViewportFrame`, a scene running beside another | **Planned** (ADR 0107). Today one camera draws the screen; switching `Workspace.CurrentCamera` shows one feed at a time. |

## Physics

| Missing | State |
|---|---|
| Springs, ropes, prismatic joints in 3D | **Not present.** The 3D joints are `BallSocketConstraint`, `HingeConstraint` and `FixedConstraint`, between two `Attachment`s, plus welds and ragdolls. The 2D layer has a `SpringConstraint2D`. |
| Concave mesh colliders | **Accepted and not implemented** — `Precise` reads back and behaves as a convex hull. |
| A sleep or wake API | **Not present.** Sleeping is real internally and is not scriptable. |
| Per-part gravity, velocity clamps | **Not present.** `Workspace.Gravity` is the knob. |

## Interface

| Missing | State |
|---|---|
| A table layout; UI shapes drawn from a path | **Not scheduled.** Grids, pages, flex, scale and the constraints are here ([Grids, pages and flex](manual:ui/layouts), [Fitting any screen](manual:ui/adapting)). |
| A drag detector's `DragRelativity`, `DragSpace` and a drag function | **Absent.** `Scriptable` and the two custom responses say where the pointer is and how far; a script does the rest. |
| Borders | **Absent**, deliberately. |
| Selection, clipboard, undo in a text field | **Not present.** Typed text, backspace and a caret. |
| A reactive UI framework | **Not the engine's.** The engine ships the instance tree. |

## Scripting

| Missing | State |
|---|---|
| A filesystem for scripts | **Not present.** No reading, no writing, no save file. |
| Most of the standard library in the game VM | **Not present yet.** Only an HTTP client is registered; JSON, paths and string helpers are not reachable from a script. |
| A localization service | **Not present.** The catalog format exists and nothing loads a game's catalog. |
| `loadstring` and friends | **Never.** Sandbox. |

## Tooling

| Missing | State |
|---|---|
| Linux, Android and dedicated-server exports from `ludwerk build` | **Here** (ADR 0104): the Export window, or `ludwerk build --target=linux\|android\|windows-server\|linux-server`. |
| iOS, the web, consoles | **Not open.** iOS is the likelier next: the engine is interpreter-only, as iOS requires. |
| An AI panel, an MCP server for AI agents | **Planned** (ADR 0108). |
| Hot-swapping a compiled asset | **Not present.** Under `ludwerk dev`, a loose file you save is reloaded on the next frame; a packed or compiled one reloads the bytes it was built into. |
| A package manager | **Not present.** |

## What to take from this

The engine is honest about its edges, and the list above is the evidence. A gap
here is a gap with a state next to it rather than a surprise waiting in week
three.

If something on this list is load-bearing for what you want to build, it is
better to know before the first line than after the hundredth.

## Where to look next

- [What Ludwerk is](manual:get-started/introduction)
- [Every deliberate divergence](manual:migrating/divergences)
