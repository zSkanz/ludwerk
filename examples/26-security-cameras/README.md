# 26 — Security cameras

The case ADR 0107 was written for: a security office at night, six cameras
around a small building, a wall of monitors showing all six at once, and a
tablet showing one of them full size
([ADR 0107](../../docs/decisions/0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md),
[the manual page](../../docs/manual/rendering/views.md)).

- **Six `Camera`s**, one high in a corner of each room, each with a
  `CameraTexture` under it drawing into `view://cam1` to `view://cam6`.
- **The monitor wall** is a `SurfaceGui` on a panel in the office: six
  `ImageLabel`s whose `Image` is those names, and a caption on each.
- **The tablet** is an `ImageLabel` in a `ScreenGui` showing the chosen feed.
  1 to 6 choose it, Tab hides it.
- **What it costs is chosen**: the wall's feeds redraw every other frame at
  320 by 180 (`UpdateInterval = 2`), and the tablet's every frame at 640 by
  360. `[render] max_views_per_frame` in `project.toml` is the ceiling; F3
  lists every view and what it cost.

```
run.bat
run.bat --headless --frames=90 --exit --screenshot=out.png
```
