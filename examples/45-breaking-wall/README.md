# 45-breaking-wall

A wall cut into pieces (ADR 0215), on its footing, and a heavy ball to roll at
it.

```
run.bat
```

- **Space** or **A** rolls a ball at the wall.
- **R** or **Y** builds it again.

Where the ball strikes, the joints there carry more than they hold and break:
the pieces fall as parts, and each joint that gives puffs dust where it was.
The rest of the wall stands on its footing.

## What is where

| | |
|---|---|
| `src/client/init.luau` | All of it: the wall and `wall:Fracture({ Pieces = 40, Seed = 7 })`, each joint's `Broken` bursting a `ParticleEmitter` where it was, the ball, the keys and the count of pieces and broken joints. |
| `i18n/` | The words on the screen, in English and Brazilian Portuguese. |

## For a picture

`--scene-data` rolls a ball with no hand on the keys:

```
run.bat --headless --frames=200 --exit --screenshot=out.png "--scene-data={\"roll\":true}"
```
