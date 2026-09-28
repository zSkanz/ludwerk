# 0128 — UI lays out in grids and pages, adapts to any screen, and is driven by a gamepad

- Status: accepted (to be built; see `docs/briefs/toolkit-kickoff.md`, F3)
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
