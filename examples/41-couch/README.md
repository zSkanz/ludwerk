# 41-couch

Several players at one screen (ADR 0205): two to four at one machine, seen
from above, each with a view of the window and an interface of their own.

```
run.bat
```

- **W A S D** or the first controller's stick moves the first player.
- **Enter** seats a second at the keyboard, on the **arrow keys**. A
  controller seats itself by being plugged in: the second controller is the
  second player, up to four.
- **Tab** or **Y** changes how the window is shared: together and apart
  across a line, side by side, one above another, the window's quarters, and
  one picture.

Walk two players apart in the first of those: one camera frames both until
they are 24 metres from each other, and then the window parts across the
direction between them, with no cut, and the line turns as they walk round
each other. With three and four it is one picture that fades into quarters.

## What is where

| | |
|---|---|
| `src/client/init.luau` | All of it: the ground, a seat a player (`AddLocalPlayer`, an `InputContext` with its `Player` and `GamepadId`, a `Camera`, a `ScreenGui` with its `Player`), and how the window is shared (`workspace.ScreenSplit`, `@engine/splitscreen`). |
| `i18n/` | The words on the screen, in English and Brazilian Portuguese. |

## For a picture

`--scene-data` puts it in a state and stands the players still, which is
how the gates take their pictures and how the cost of a view was measured:

```
run.bat --headless --frames=40 --exit --screenshot=out.png "--scene-data={\"players\":4,\"mode\":\"Grid\",\"apart\":30}"
```

`players` is 1 to 4; `mode` is `Dynamic`, `Columns`, `Rows`, `Grid` or
`None`; `apart` is how far from the middle each stands, in metres, and
`turn` turns them round it in degrees; `dense` stands a tower every fourth
metre instead of every twelfth -- a thousand of them; `line` draws the line
between views wide and magenta.
