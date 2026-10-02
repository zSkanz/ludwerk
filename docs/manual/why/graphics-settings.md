# Graphics settings belong to the player

`Lighting` is a service a script writes because it is part of the world.
Shadow resolution, render scale, bloom, the window's mode and the frame cap are
not part of any world -- and since ADR 0147 a script can write those too,
through `GraphicsService`. What changed is who a script is speaking for.

The line between the two has not moved:

> **`Lighting` describes the world. Graphics settings describe the machine it is
> being shown on.**

`Lighting.ClockTime` is a fact about the place — it is eight in the morning
there. `ShadowResolution = 2048` is a fact about a graphics card.

## Whose decision it is

A scene must not decide a stranger's GPU budget. A world author knows what their
scene looks like; they do not know whether it is being played on a laptop with
integrated graphics. The person who does know is the person running it.

That is why the settings were closed to scripts at first (ADR 0044): the only
script anybody could imagine writing them was the scene's. It left a game with
nothing to build an options menu on -- and an options menu is exactly the
player deciding. So the settings are open to scripts, and the order of who is
listened to is what keeps the decision the player's:

1. **the command line** -- whoever started this run, by hand;
2. **a script, this session, and the player's saved choices** -- the menu the
   player is using, and what they chose last time;
3. **the project's file** -- the author's defaults;
4. **the level** those chose, and the engine's own defaults under it.

The author's word is a default. The player's is over it, and stays over it:
`SaveAsync` keeps their choices in their own folder, and a game starts in them.

## What a script must not do with them

**Put them into the world.** Nothing in the graphics family reaches the
simulation: a world hashes the same at every level, a trace recorded at one
replays at another, and none of these properties is saved with a scene or sent
to another player. That is still verified -- the determinism replay runs at two
settings.

`GraphicsService` does not break that by itself, and a script can: one that
spawns fewer enemies when `QualityLevel` is `Low` has made two players at two
settings play different games. Read the settings to show a menu. To adapt what
the game *does*, read the frame it produced:

```luau
--!strict
local DebugService = game:GetService("DebugService")

if DebugService:GetStat("FrameTimeMs") > 20 then
    reduceEffectDensity()
end
```

In a multiplayer game `GraphicsService` is the player's side's: a dedicated
server has no display, reads the defaults, and a write there raises.

## Still not scene state

A scene file holds no graphics setting, and the editor's Properties panel has
none to offer. A game's defaults are in `project.toml`, where a person reading
the project can see them.

And one rule that is not implied by "later wins": **a level named on the
command line replaces the file's per-key entries as well as its level.** A
file's `shadow_resolution` refines the level *that file names*; `--quality=low`
says that level is not available here, so carrying the refinement across would
hand a weak machine the heaviest dial in the file while everything else was
turned down.

## Where to look next

- [Graphics and display settings](manual:rendering/settings) — for a player, by script, in a project
- [Graphics quality settings](manual:rendering/quality) — every key, its range and the levels
- [Lighting and the sky](manual:rendering/lighting) — what a scene *does* own
