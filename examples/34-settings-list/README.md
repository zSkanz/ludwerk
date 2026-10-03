# 34 — Settings list

A settings list as a phone has one, and not one line of scrolling code (the
manual's [Buttons and interaction](../../docs/manual/ui/interaction.md) and
[Layout](../../docs/manual/ui/layout.md)):

```
run.bat
run.bat --width=1280 --height=720
```

- **Throw it**: drag the list and let go while moving, and it glides on and
  slows. A press on it while it glides stops it and presses nothing.
- **Pull past an end**: it gives, less the further you pull, and springs back.
  A throw that reaches the end bounces.
- **Wallpaper** is a strip inside the list that scrolls sideways. A drag that
  starts down scrolls the list, one that starts sideways the strip, and each
  keeps its way until the finger lifts.
- **The bar**: drag the thin bar on the right, or press beside its thumb.
- **With no pointer**: the arrows or a d-pad walk down the rows, and the list
  scrolls to keep the selected one in sight. Enter or `ButtonA` turns a switch.
- **The title** scrolls back to the top: a script writing `CanvasPosition`.

The list's canvas is `AutomaticCanvasSize = Enum.AutomaticSize.Y`, so it is as
tall as its rows; the strip's is automatic across. The script is
`src/client/init.luau`.
