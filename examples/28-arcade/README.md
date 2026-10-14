# 28 — Arcade

Two arcade cabinets in a dark hall, each running a small game in a world of its
own
([ADR 0107 §3](../../docs/decisions/0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md),
[the manual page](../../docs/manual/rendering/views.md)).

- **Each cabinet is a `SubWorld`.** `Load()` starts
  `content/scenes/arcade.scene.json` in a world with its own instances,
  scripts, physics and clock. Its camera draws into `view://arcade1` or
  `view://arcade2`, and an ordinary `ImageLabel` on the cabinet's screen shows
  it. Until somebody plays, each cabinet plays itself.
- **The game is the scene's own code**, `src/scenes/arcade/server/game.luau`.
  The hall's code in `src/client/` does not run inside it, and the game cannot
  see the hall.
- **Nothing crosses but what is sent.** Press E at the left cabinet: the hall
  sends it a coin (`Send`) and passes A and D or the arrows on as the game's
  `Move` action (`SetInputState`). It then writes the score the game sends back
  (`Received`) on the cabinet's marquee. Each cabinet also gets a seed of its
  own, so the two are not playing the same game.

`project.toml` sets `[render] max_sub_worlds = 2`: two cabinets, two worlds.

```
run.bat
run.bat --headless --frames=150 --exit --screenshot=out.png
```
