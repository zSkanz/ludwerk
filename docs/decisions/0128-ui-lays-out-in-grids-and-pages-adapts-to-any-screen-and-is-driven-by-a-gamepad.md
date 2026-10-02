# 0128 — UI lays out in grids and pages, adapts to any screen, and is driven by a gamepad

- Status: accepted, and built (`docs/briefs/toolkit-kickoff.md`, F3; "As built" below)
- Date: 2026-09-27
- Decided by: the owner, on 2026-09-27, approving the UI items of a survey of
  public documentation (R7) of the platform whose API shape this engine follows.
- Builds on: [0040](0040-udim2-layout-is-arithmetic-not-a-solver.md) (UDim2
  layout is arithmetic, not a solver), [0110](0110-a-gradient-colours-and-a-stroke-outlines-a-ui-element.md)
  (the last UI appearance modifiers), [0107](0107-a-camera-draws-into-a-texture-a-frame-draws-its-own-instances-and-a-scene-runs-beside-another.md)
  (drawing into a texture).

## Context

The UI has `Frame`, text and image labels and buttons, `TextInput`,
`ScrollFrame`, `ViewportFrame`, three kinds of `Gui`, and as modifiers
`UIListLayout`, `UIPadding`, `UICorner`, `UIGradient` and `UIStroke`. An
inventory grid, a paged tutorial, a HUD that fits a phone and a monitor, a
window that fades as one, a slider dragged by a finger, and a menu driven by a
gamepad are each hand-written today.

## Decision

Every rule stays inside ADR 0040: layout is arithmetic, in one pass per
container, never a solver.

### 1. Layouts

- **`UIGridLayout`**: `CellSize`, `CellPadding`, `FillDirection`,
  `FillDirectionMaxCells`, `StartCorner`, `SortOrder`, horizontal and vertical
  alignment; `AbsoluteContentSize`.
- **`UIPageLayout`**: pages side by side, one shown; `Animated`, `Circular`,
  `EasingStyle`, `EasingDirection`, `TweenTime`, `Padding`, `ScrollWheelInputEnabled`,
  `TouchInputEnabled`, `GamepadInputEnabled`; `CurrentPage`, `JumpTo(page)`,
  `JumpToIndex(i)`, `Next()`, `Previous()`; `PageEnter`, `PageLeave`,
  `Stopped` signals.
- **Flex in `UIListLayout`**: `Wraps`, `HorizontalFlex` and `VerticalFlex`
  (`None`, `Fill`, `SpaceAround`, `SpaceBetween`, `SpaceEvenly`), and
  **`UIFlexItem`** on a child: `FlexMode` (`None`, `Grow`, `Shrink`, `Fill`,
  `Custom`), `GrowRatio`, `ShrinkRatio`, `ItemLineAlignment`.

### 2. Adapting to the screen

- **`UIScale`** (`Scale`): scales an element and its descendants, drawn and
  pressed; animatable by a tween.
- **`UIAspectRatioConstraint`**: `AspectRatio`, `AspectType` (`FitWithinMaxSize`,
  `ScaleWithParentSize`), `DominantAxis`.
- **`UISizeConstraint`**: `MinSize`, `MaxSize`.
- **`UITextSizeConstraint`**: `MinTextSize`, `MaxTextSize` (for scaled text).
- Constraints apply after an element's own size and before its children are laid
  out, as a clamp — arithmetic.

### 3. A group drawn as one

- **`CanvasGroup`**: a `Frame` whose descendants are drawn into a texture and
  composited once, with `GroupTransparency` and `GroupColor`. A whole window
  fades as one instead of its parts fading through each other. It uses the view
  texture registry of ADR 0107 and counts against its budget; it is redrawn only
  when something inside it changes.

### 4. Dragging UI

- **`UIDragDetector`**: child of a `GuiObject`; `DragStyle` (`TranslatePlane`,
  `TranslateLine`, `Rotate`, `Scriptable`), `DragAxis`, `BoundingUI` (stays
  inside another element), `DragRelativity`, `DragSpace`, `MinDragTranslation`,
  `MaxDragTranslation`, `MinDragAngle`, `MaxDragAngle`, `ResponseStyle`,
  `Enabled`; `DragStart`, `DragContinue`, `DragEnd` signals. Mouse and touch.

### 5. Driven by a gamepad or the keyboard

- `GuiObject.Selectable`, `NextSelectionUp`, `NextSelectionDown`,
  `NextSelectionLeft`, `NextSelectionRight` (empty means the nearest in that
  direction, computed from positions), `SelectionImageObject`.
- `UIService.SelectedObject` (the focused element), `UIService.AutoSelect`
  (when a gamepad is used, focus the first selectable element), and a
  `SelectionChanged` signal.
- The d-pad, the left stick and the arrow keys move the selection and
  `ButtonA`/`Enter` activate it, through an engine-owned `InputContext` a game's
  own contexts can override.
- The default selection look is a themed outline (a `UIStroke`); a game can
  replace it.

## Consequences

- An inventory, a shop, a tutorial, a HUD for a phone and a PC, and a menu for a
  controller, without layout code.

## Not decided here

- A table layout.
- Path-drawn UI shapes.

## As built, 2026-10-02

The classes are `UIObject`'s, not `GuiObject`'s: that is what this engine
calls the base of everything on a screen.

- **One layout to a parent**: a list, a grid or pages, and the first it has.
- **A hidden child keeps no room** in any of them, nor in a parent that sizes
  to its content (D478). It kept a slot in a list until now.
- **Defaults are the engine's own** where a number had to be chosen: a page
  slides for 0.3 seconds on `Quad`, `Out`; a grid's cells are 100 with 5
  between. `UISizeConstraint.MaxSize` is **zero for no limit** -- a number a
  scene file can hold -- and takes `math.huge` as the same.
- **`AspectType.ScaleWithParentSize`** takes the parent's whole extent on the
  dominant axis and lets the other be what the shape makes it, which may be
  more than the parent has. `FitWithinMaxSize` is the largest box of the shape
  inside the element's own size, and there the dominant axis changes nothing.
- **A size limit has the last word** over a shape on the same element.
- **Flex is one pass** (ADR 0040): room left over goes to the children that
  grow, by their shares; with none, to the layout's own flex. A child that
  shrinks to nothing takes no more and the rest is not shared out again.
  Across the lines, flex acts on a wrapped list's lines.
- **`UIScale` is applied after the layout**: the element takes `Scale` times
  its size, its insides are laid out unscaled, and every rectangle under it is
  then multiplied about its corner -- with `unitScale`, which is how text,
  corners and strokes follow. A scaled `ScreenGui` multiplies again.
- **`UITextSizeConstraint`** acts on `TextScaled` text and on nothing else.
- **`CanvasGroup` has a registry of its own**, beside the view textures'
  rather than rows in it: a view is a camera's and is paced by the view
  budget, and a group's picture must be this frame's or the interface is
  wrong. It keeps the same two kinds of limit -- 32 pictures, 4096 pixels a
  side -- and past either a group draws as a frame. The picture holds colour
  already multiplied by its alpha, so the quad that shows it is drawn by a
  second pipeline of the same shader that blends one times the source; no
  shader changed. It is drawn again when a hash of the quads in it moves, when
  it holds a `ViewportFrame` or a gradient, or when a group inside it was. On
  a world canvas a group is a frame.
- **`UIDragDetector`** has `DragStyle`, `ResponseStyle`, `DragAxis`,
  `BoundingUI`, the two translation limits, the two angle limits and
  `Enabled`, and reports `DragUDim2` and `DragRotation`. **`DragRelativity`
  and `DragSpace` are not built**: they say what a drag function returns, and
  there is no drag function -- `Scriptable` and the custom responses hand the
  numbers to a script. A press that became a drag fires no `Activated`.
- **Selection** is `UIObject.Selectable` (as `Active` is: what the object is
  until it is told), the four `NextSelection*`, `SelectionImageObject`,
  `SelectionGained` and `SelectionLost`; and `UIService.SelectedObject`,
  `AutoSelect` and `SelectionChanged`. Nearest is the least of "distance along
  the direction plus twice the distance off it" among what is ahead.
- **The engine-owned `InputContext` is a rule, not an Instance** -- as the
  pointer's claim is (ADR 0041): a key, button or stick the game has bound in
  an enabled context is the game's, and the interface is told nothing about
  it. So `AutoSelect` never takes the arrows from a character that walks with
  them, and a menu is driven by them once the game's play context is off.
  `engine/app/src/ui_navigation.cpp` reads the events; a held d-pad or stick
  repeats after 0.4 seconds and then every 0.12.
- **A `ScrollFrame` is scrolled by hand** (D477) -- the wheel and a dragging
  finger -- which it never was, and its canvas is never smaller than the frame
  on either axis (D479). Both were found here, by the example.
- **Nothing here travels**: every class is screen-space UI and excluded from
  the wire with the rest. The properties added to classes that existed --
  selection on `UIObject`, flex on `UIListLayout` -- say nothing in the world
  hash at their defaults, so no trace moved.
- **Tests**: `adaptive_layout_tests.cpp` (every layout and constraint against
  positions worked out by hand), `selection_tests.cpp` (selection, drag,
  scroll, pages by hand), `canvas_group_tests.cpp` (the draw list),
  `ui_navigation_tests.cpp` (the host's reading of the devices),
  `ui_layouts.spec.luau` (the API), and `ui_layouts_gate` -- twenty-four
  probes of a frame drawn on a real device, the canvas group's among them.
