# 27 — Mirrors and portals

A gallery with a mirror on its back wall and a door that opens onto a garden
room across the map, seen by a camera walking slowly past
([ADR 0107](../../docs/decisions/0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md),
[the manual page](../../docs/manual/rendering/views.md)).

- **The mirror** is `views.mirror(glass)` from `@engine/views`: a camera
  reflected through the glass every tick, its picture on the glass's front
  face. Walk past it and the room in it moves as a room in a mirror does.
- **The door** is `views.portal(door, gardenDoor)`: what is in front of the
  garden's pane shows in the gallery's.
- Both use `Camera.ClipPlane`, so what stands behind the glass is not in the
  picture, and both are exact from any angle: the camera looks straight
  through the glass and the glass is cut out of its picture.

```
run.bat
run.bat --headless --frames=120 --exit --screenshot=out.png
```
