# Grids, pages and flex

Three ways to arrange children without writing where each one goes
(ADR 0128). Like `UIListLayout`, each is a **modifier**: parented beside the
things it arranges, under the same parent. A parent takes one layout -- a list,
a grid or pages -- and the first it has is the one.

Every rule here is arithmetic on the parent's size and the children's, in one
pass. A child that is not `Visible` is not there: it keeps no cell, no line and
no page.

## UIGridLayout

Equal cells, a line at a time. An inventory, a shop, a level select:

```luau
local slots = Instance.new("Frame")
slots.Size = UDim2.new(0, 400, 0, 300)
slots.Parent = screen

local grid = Instance.new("UIGridLayout")
grid.CellSize = UDim2.fromOffset(100, 100)
grid.CellPadding = UDim2.fromOffset(10, 10)
grid.Parent = slots

for index = 1, 7 do
	local slot = Instance.new("TextButton")
	slot.Text = tostring(index)
	slot.Parent = slots
end
```

Three cells fit a row of 400 (three cells and two gaps are 320; a fourth would
need 430), so the seven are rows of three, three and one.

| Property | What it does |
|---|---|
| `CellSize` | The size of every cell. Scale is a fraction of the parent: `UDim2.fromScale(0.25, 0.25)` is four across at any size. A child's own `Size` is not read. |
| `CellPadding` | The gap between cells, across and down. Between, never around: `UIPadding` is around. |
| `FillDirection` | `Horizontal` fills rows; `Vertical` fills columns. |
| `FillDirectionMaxCells` | The most cells in a line. Zero is "as many as fit", and a line never holds more than fit. |
| `StartCorner` | The corner the first cell goes in: `TopLeft`, `TopRight`, `BottomLeft`, `BottomRight`. |
| `SortOrder` | `LayoutOrder` or `Name`, as a list's. |
| `HorizontalAlignment`, `VerticalAlignment` | Where the whole block of cells sits in the parent. |
| `AbsoluteContentSize` | Read-only: how much room the cells take, in pixels. |

**A grid that scrolls** is a grid in a `ScrollFrame` whose canvas is as tall as
the grid. `AbsoluteContentSize` is in pixels and `CanvasSize` in the screen's
own units, which differ when the `ScreenGui` has a `ReferenceHeight`:

```luau
local unit = UIService.ViewportSize.Y / screen.ReferenceHeight
list.CanvasSize = UDim2.fromOffset(0, grid.AbsoluteContentSize.Y / unit)
```

A parent with `AutomaticSize` is as large as its grid. Along the line it has all
the room it likes, so it is one line -- or `FillDirectionMaxCells`.

## UIPageLayout

The children side by side as pages, one of them shown. A tutorial, a set of
tabs, a carousel:

```luau
local body = Instance.new("Frame")
body.Size = UDim2.fromScale(1, 1)
body.ClipsDescendants = true
body.Parent = screen

local pages = Instance.new("UIPageLayout")
pages.Parent = body

for index = 1, 3 do
	local page = Instance.new("Frame")
	page.Size = UDim2.fromScale(1, 1)
	page.LayoutOrder = index
	page.Parent = body
end

nextButton.Activated:Connect(function()
	pages:Next()
end)
```

The other pages are beside the shown one, outside the parent's box. **Turn
`ClipsDescendants` on**, or they show.

A page keeps its `Size` and loses its `Position`. A page smaller than the parent
sits where `HorizontalAlignment` and `VerticalAlignment` put it -- the middle,
until you say otherwise -- and `Padding` is the gap between pages.

**Turning.** `Next()`, `Previous()`, `JumpTo(page)` and `JumpToIndex(index)`
(counted from 0, in the layout's order). `CurrentPage` has changed by the time
the call returns; the pages then slide for `TweenTime` along `EasingStyle` and
`EasingDirection`, or are there at once when `Animated` is off. Past either end
is that end, unless `Circular`: then the page after the last is the first, and
`Next` at the last slides on rather than back through them all.

Hands turn pages too, each with a switch:

| Switch | What turns the page |
|---|---|
| `TouchInputEnabled` | A swipe across the pages, by a finger or a dragged mouse: a sixth of the pages' extent, and at least 24 pixels. A press that became a swipe does not press what is under it. |
| `ScrollWheelInputEnabled` | The wheel over the pages. A `ScrollFrame` under the pointer that can still move takes the wheel first. |
| `GamepadInputEnabled` | A gamepad's shoulder buttons: the pages the selection is on, or the first page layout shown. |

**Signals.** `PageLeave(page)` and `PageEnter(page)` when `CurrentPage`
changes, and `Stopped(page)` when the slide ends -- at once when not animated.

Pages inside a page work: turn `GamepadInputEnabled` off on the inner one, so
the shoulder buttons mean one thing.

## Flex in UIListLayout

A list places its children at their own sizes and leaves what is over. Flex says
what happens to it.

**On the layout**, per axis -- `HorizontalFlex` and `VerticalFlex`:

| `Enum.UIFlexAlignment` | Room left over |
|---|---|
| `None` | Stays where the alignment leaves it. |
| `Fill` | Every child grows by the same amount. |
| `SpaceBetween` | Goes between the children; none at the ends. |
| `SpaceAround` | A share around each child: a whole share between two, half at each end. |
| `SpaceEvenly` | Equal gaps, the ends included. |

```luau
local row = Instance.new("UIListLayout")
row.FillDirection = Enum.FillDirection.Horizontal
row.HorizontalFlex = Enum.UIFlexAlignment.Fill
row.Parent = tabs -- three tabs now share the bar
```

Along the line that is plain. Across it, flex acts on the **lines** of a list
that `Wraps`: `Fill` makes each line deeper, and the space values put the room
between lines.

**On a child**, a `UIFlexItem` parented to it:

| `FlexMode` | The child |
|---|---|
| `None` | Keeps its size. |
| `Grow` | Takes room that is left over. |
| `Shrink` | Gives up room when the line is too long. |
| `Fill` | Both. |
| `Custom` | By `GrowRatio` and `ShrinkRatio`: one at 2 beside one at 1 takes two thirds. |

A child that grows takes the room before the layout's own flex sees it: a search
box with `Grow` between two buttons fills the bar, whatever `HorizontalFlex`
says.

**Across the line**, `ItemLineAlignment` on the layout places every child --
`Start`, `Center`, `End`, or `Stretch`, which makes each as tall as a row or as
wide as a column. A child's `UIFlexItem.ItemLineAlignment` says otherwise for
that child. `Automatic`, the default, is the layout's own alignment, as before.

`UIListLayout.AbsoluteContentSize` is what the children take **as they asked**,
before any flex -- what a scrolling list is sized from.

One pass: a child that shrinks to nothing takes no more, and the rest is not
shared out again.

## Where to look next

- [Layout with UDim2](manual:ui/layout) -- `UIListLayout`, `UIPadding`, `AutomaticSize`
- [Fitting any screen](manual:ui/adapting) -- scale, shape and size limits
- [A gamepad and the arrow keys](manual:ui/selection)
- [`UIGridLayout`](api:UIGridLayout) · [`UIPageLayout`](api:UIPageLayout) ·
  [`UIListLayout`](api:UIListLayout) · [`UIFlexItem`](api:UIFlexItem)
- `examples/32-menus` -- all three in one window
