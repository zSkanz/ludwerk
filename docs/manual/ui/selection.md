# A gamepad and the arrow keys

A pointer says which button it means by being on it. A gamepad cannot, so the
interface keeps a **selection**: one object, outlined, that the d-pad, the left
stick and the arrow keys move and that `ButtonA` or Enter presses (ADR 0128).

```luau
local UIService = game:GetService("UIService")

-- A menu that opens says where a gamepad starts.
UIService.SelectedObject = playButton

playButton.Activated:Connect(startGame) -- a click, a tap, ButtonA, Enter
```

`Activated` is the same event a click fires. A menu written for a mouse is a
menu for a gamepad the moment something is selected.

## What can be selected

`UIObject.Selectable`. **Until you write it, it is what the object is**: a
`TextButton`, an `ImageButton` and a `TextInput` are selectable, and nothing
else is. Write `true` on a frame that acts as a button, or `false` on a button
a gamepad should skip.

Selecting a `TextInput` and pressing gives it the keyboard, as a click would;
while it is being typed into, the arrows are its caret's.

## Where the selection goes

A step in a direction goes to the **nearest selectable object that way**, found
from where the objects are on the screen: ahead of the selected one, and the
least far -- its distance along the direction plus twice its distance off it,
so the button straight below wins over a nearer one that is mostly to the side.
Hidden objects and disabled screens are passed over. With nothing that way, the
selection stays.

Say otherwise with `NextSelectionUp`, `NextSelectionDown`, `NextSelectionLeft`
and `NextSelectionRight` on an object -- to wrap a list from its last row to
its first, or to step over a decoration:

```luau
last.NextSelectionDown = first
first.NextSelectionUp = last
```

A held d-pad or stick steps again after 0.4 seconds and then every 0.12; a held
arrow repeats as the system's keyboard does.

## AutoSelect

`UIService.AutoSelect`, on by default, is the engine looking after the
selection when the game does not:

- with nothing selected, the first step of a d-pad, a stick or an arrow selects
  the **first selectable object on the topmost screen**;
- a press of the pointer clears the selection, so a player who picks up the
  mouse sees no outline.

Off, the selection is whatever a script last wrote.

## The game's own bindings come first

**A key, button or stick the game has bound in an enabled `InputContext` is
the game's**, and the interface is not told about it. A character walked with
the arrows keeps walking with a button on the screen; nothing is selected by
accident.

So a menu is driven by the same keys when the game is not using them: turn the
play context off while the menu is open (`context.Enabled = false`), which most
games do anyway, or bind play to other keys than the menu's.

## Signals and the look

- `UIService.SelectionChanged(selected)` -- the object, or nil.
- `UIObject.SelectionGained` and `SelectionLost` on the object itself: where a
  button grows or plays a sound.

The selected object is outlined a little outside its box, following its
`UICorner`. To draw something else, set `SelectionImageObject` to a `UIObject`
-- a frame with a `UIStroke`, a nine-slice picture: its background, picture,
corners, gradient and strokes are drawn over the selected object's box in
place of the outline. It needs no parent, and its children are not drawn.

## Pages

A gamepad's shoulder buttons turn a `UIPageLayout`'s page: the pages the
selection is on, or the first page layout shown. See
[Grids, pages and flex](manual:ui/layouts).

## Where to look next

- [Buttons and interaction](manual:ui/interaction) -- `Activated`, dragging, scrolling
- [Actions and contexts](manual:input/actions) -- what "bound in an enabled context" means
- [`UIService`](api:UIService) · [`UIObject`](api:UIObject)
- `examples/32-menus`
