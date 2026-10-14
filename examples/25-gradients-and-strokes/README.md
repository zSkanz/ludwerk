# 25 — Gradients and strokes

Every shape of `UIGradient` and every kind of `UIStroke` on one screen
([ADR 0110](../../docs/decisions/0110-a-gradient-colours-and-a-stroke-outlines-a-ui-element.md),
[the manual page](../../docs/manual/ui/gradients-and-strokes.md)):

- **Top row** -- a health bar (linear, three colours) with a shine sliding
  across it; a radial glow; a conical progress ring filling; stripes from
  `Repeat` and from `Mirror`.
- **Middle row** -- border strokes: outer, centre and inner; round, bevel and
  miter corners; a double border from two strokes; a rounded button whose
  stroke carries its own gradient.
- **Bottom row** -- text strokes: an outlined title, hollow lettering, a
  scaled stroke, and rich text's `<stroke>` tag.
- **In the world** -- a sign on a wall whose panel is a gradient under a
  stroke, drawn by the world's UI pass.

```
run.bat
run.bat --headless --frames=60 --exit --screenshot=out.png
```
