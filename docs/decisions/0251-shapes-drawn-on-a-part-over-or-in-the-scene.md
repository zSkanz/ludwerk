# 0251 — Shapes drawn on a part, over the scene or in it

- Status: accepted and built
- Date: 2026-10-19
- Builds on: [0129](0129-highlight-beam-and-trail.md) (`Highlight`),
  [0250](0250-how-a-headsets-eyes-are-drawn-is-the-projects-to-set.md) §7
  (the world's interface drawn as the display shows it)

## Context

A game marks things in the world all the time: the box a builder is about to
place, a ring under a selected unit, an arrow over a quest giver, the outline
of what the player is pointing at. Today it can only do that with parts --
which collide, cast shadows, are lit and exposed with the scene, and are
hidden by every wall -- or with a `Highlight`, which tints a whole model and
draws no shape of its own. `BillboardGui` and `SurfaceGui` draw over the
scene, but only an interface's rectangles.

What is missing is a family of simple shapes attached to a part, drawn in
plain colors, that can be drawn over everything when the game says so.

## Decision

1. **Shape adornments**, each an `Instance` placed by an `Adornee` (a part,
   or a model's primary part) and a `CFrame` relative to it:
   `BoxHandleAdornment` (`Size`), `SphereHandleAdornment` (`Radius`),
   `CylinderHandleAdornment` (`Radius`, `Height`), `ConeHandleAdornment`
   (`Radius`, `Height`), `LineHandleAdornment` (`Length`, `Thickness`) and
   `ImageHandleAdornment` (`Image`, `Size`, facing along its `CFrame`). Each has `Color3`, `Transparency`, `Visible`,
   `AlwaysOnTop` and `ZIndex`.
2. **And outlines of a part or a model**: `SelectionBox` (the adornee's
   bounding box: its twelve edges, and its faces) and `SelectionSphere` (its
   bounding sphere, drawn as the circle it shows the camera, with a rim),
   each with `LineThickness`, `Color3` for the edges, and `SurfaceColor3` and
   `SurfaceTransparency` for what is inside them (1: only the edges). An
   outline is drawn over everything, as a selection is.
3. **Drawn as the display shows them**, as the world's interface is: not lit,
   not exposed and not through the curve, so a red marker is the red it was
   set to by day and by night, blended in the display's linear light. They
   cast no shadow and nothing collides with them.
4. **`AlwaysOnTop` false**: tested against the scene's depth, hidden by what
   is in front. **True**: drawn after the scene with no depth test, in
   `ZIndex` order (higher over lower), then from far to near. In a headset
   both eyes draw them, from each eye's own view.
5. **One draw a shape a frame, whatever their number**: every adornment of a
   kind is an instance of one mesh -- the primitives the parts are drawn
   with, and a cone made the same way -- with its transform, color and
   opacity, so a thousand markers are a handful of draws.
6. **In the world tree, and on the wire**, as parts are: a server that
   marks an objective, a capture zone or a target shows it to every player,
   and an adornment a client's script makes stays on that client, as any
   instance a client makes does. They are looks, not state: none of them is
   in the world's hash.

## Consequences

- A builder's ghost box, a selection outline and a waypoint arrow are a few
  lines of a script, with no part to keep out of the physics.
- What they cannot do yet: report a pointer entering or leaving them, or a
  click on them. Pointer events are a later step, after they are drawn right.
