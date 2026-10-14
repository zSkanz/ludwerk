# What is not here

Written so you find out now rather than three weeks in.

Everything below is genuinely absent. Where something is *planned*, it says so;
where it is not, it says that too.

## The platform

| Missing | State |
|---|---|
| A players service, avatars | **Not planned.** This is an engine, not a hosted platform. |
| Accounts, friends, achievements, a store | **Through a platform, not the engine.** `IdentityService`, `SocialService`, `AchievementService` and `StoreService` reach a platform's own services through an optional provider; Xbox on Windows is the one there is. See [Platform services](manual:guides/platform-services). |
| Data stores | **Not planned.** A game saves on the player's machine with `SaveService`, or in a platform's cloud with `CloudSaveService`; anything shared between players is a backend you write. |
| Analytics | **Not planned.** |
| Matchmaking and hosted servers | **Not planned.** A match is hosted by a player's machine (`NetworkService:Host()`, or `--host`) or a server you run (`--serve`), and a script joins one with `NetworkService:Join(address)`; finding one is your backend's job. |

Replication and a game's own messages are here: see
[Multiplayer](manual:guides/multiplayer). Anything that outlives a match is one
HTTP client and your own server: see [Talking to a backend](manual:guides/backend).

## Rendering and world content

| Missing | State |
|---|---|
| Complex-script shaping | **Not scheduled.** A label lays its codepoints out left to right, so Arabic, Devanagari and Thai do not join. Rich text is here: `TextLabel.RichText`. |
| An HDR panorama sky | **Not present.** A `Sky` takes six images, and reflections come from it; an HDR panorama is not a format it reads. See [Atmosphere and the sky](manual:rendering/atmosphere-and-sky). |
| Motion blur, film grain, lens flare, a vignette | **Not present.** Depth of field, color correction, bloom, blur and sun rays are here, as effects under `Lighting` (see [The post chain](manual:rendering/post)). |

## Physics

| Missing | State |
|---|---|
| Concave mesh colliders | **Accepted and not implemented** — `Precise` reads back and behaves as a convex hull. |
| A sleep or wake API | **Not present.** Sleeping is real internally and is not scriptable. |
| Per-part gravity, velocity clamps | **Not present.** `Workspace.Gravity` is the knob. |

## Interface

| Missing | State |
|---|---|
| A table layout; UI shapes drawn from a path | **Not scheduled.** Grids, pages, flex, scale and the constraints are here ([Grids, pages and flex](manual:ui/layouts), [Fitting any screen](manual:ui/adapting)), and so are rounded corners, gradients and strokes ([Gradients and strokes](manual:ui/gradients-and-strokes)). |
| A `UIDragDetector`'s `DragRelativity`, `DragSpace` and a drag function | **Absent.** `Scriptable` and the two custom responses say where the pointer is and how far; a script does the rest. |
| A proximity prompt, a drag detector for parts in the world | **Absent, deliberately.** Each is a few lines of a game's own over a `ClickDetector` and an input action; see [Clicks, prompts and drags](manual:input/interactions). |
| A reactive UI framework | **Not the engine's.** The engine ships the instance tree. |

## Scripting

| Missing | State |
|---|---|
| A filesystem for scripts | **Not present.** A script cannot read or write files. Saves go through `SaveService`, which takes slot names and never paths. |
| Most of the standard library in the game VM | **Not present.** `@std/net` and `@std/json` are reachable from a script; paths, files and the string and table helpers are not. See [What a script may do](manual:concepts/sandbox). |
| `loadstring` and friends | **Never.** Sandbox. |

## Tooling

| Missing | State |
|---|---|
| iOS, the web, consoles | **Not present.** A game runs on Windows, Linux, macOS and Android. The web is on the roadmap. The engine runs scripts in an interpreter, as iOS requires, so nothing in its design rules iOS out. |
| Hot-swapping a compiled asset | **Not present.** Under `ludwerk dev`, a loose file you save is reloaded on the next frame; a packed or compiled one reloads the bytes it was built into. |
| A package manager | **Not present.** It is on the roadmap. |

## What to take from this

The engine is honest about its edges, and the list above is the evidence. A gap
here is a gap with a state next to it rather than a surprise waiting in week
three.

If something on this list is load-bearing for what you want to build, it is
better to know before the first line than after the hundredth.

## Where to look next

- [What Ludwerk is](manual:get-started/introduction)
- [Every deliberate divergence](manual:migrating/divergences)
