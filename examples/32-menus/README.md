# 32 — Menus

One window for every screen and every hand
([ADR 0128](../../docs/decisions/0128-ui-lays-out-in-grids-and-pages-adapts-to-any-screen-and-is-driven-by-a-gamepad.md);
the manual's [Grids, pages and flex](../../docs/manual/ui/layouts.md),
[Fitting any screen](../../docs/manual/ui/adapting.md) and
[A gamepad and the arrow keys](../../docs/manual/ui/selection.md)):

- **Inventory** -- forty-two slots in a `UIGridLayout` inside a `ScrollFrame`:
  as many across as the window has room for, scrolled by the wheel or a finger.
  Each item is kept square by a `UIAspectRatioConstraint`.
- **How to play** -- three cards in a `UIPageLayout` that goes round, turned by
  Back and Next, a swipe or the wheel; the titles are scaled text held between
  two sizes by a `UITextSizeConstraint`.
- **Options** -- a slider whose thumb is dragged by a `UIDragDetector` along
  its track and kept on it, and three toggles.

The three tabs share their bar by flex and turn the window's own
`UIPageLayout`, as a swipe, the wheel and a gamepad's shoulder buttons do. The
window is a `CanvasGroup`, so it fades in as one picture, with a `UIScale` that
pops it open, and a `UISizeConstraint` keeps it usable from a phone held
upright to a wide monitor.

**With no pointer**: the arrows, the d-pad or the left stick move the outline
between buttons, and Enter or `ButtonA` presses the one it is on. A press of
the mouse puts the outline away.

```
run.bat
run.bat --width=640 --height=960
run.bat --width=1920 --height=1080
run.bat --headless --frames=60 --exit --screenshot=out.png
```

No layout code: nothing in the script computes where anything goes.
