// A `UIPageLayout`'s pages and the turning of them (ADR 0128).
//
// Here rather than in `ui` because two modules turn a page and neither may
// reach the other: `ui` on a swipe, the wheel and a gamepad's shoulder button,
// and `script` on `Next`, `Previous`, `JumpTo` and `JumpToIndex`. What a turn
// does to the world -- which page is current, which events say so, where the
// slide starts from -- is a fact about the tree and nothing else, so it lives
// with the tree. How the pages are laid out and how the slide is drawn is `ui`'s.
#pragma once

#include <vector>

#include "engine/core/id.h"
#include "engine/core/types.h"

namespace engine::scene {

class World;

// `Enum.SortOrder`, and "document order" for a parent with no layout.
inline constexpr core::i32 kUiSortByName = 0;
inline constexpr core::i32 kUiSortByLayoutOrder = 1;
inline constexpr core::i32 kUiSortByDocument = -1;

// The `UIObject` children of `parent` a layout arranges, in the order it
// walks them: the visible ones, in document order or re-sorted -- stably, so a
// tie keeps document order (R10).
void uiChildrenInOrder(const World& world, core::InstanceId parent, core::i32 sortOrder,
                       std::vector<core::InstanceId>& out);

// The pages a `UIPageLayout` arranges, in its order.
void pagesOf(const World& world, core::InstanceId layout, std::vector<core::InstanceId>& out);

// Turns a `UIPageLayout` to the page at `index` in its order: `CurrentPage`
// changes at once, `PageLeave` and `PageEnter` fire, and the pages start to
// slide there when the layout is `Animated` -- `Stopped` fires now when it is
// not. Past either end is that end, or round it when `Circular`. `direction`
// is which way round to slide when the pages go round: positive forwards,
// negative backwards, zero the shorter. False when nothing changed.
bool turnPage(World& world, core::InstanceId layout, core::i32 index, core::i32 direction = 0);

// `step` pages on from `CurrentPage`: 1 is `Next`, -1 is `Previous`.
bool stepPage(World& world, core::InstanceId layout, core::i32 step);

// Marks the `ScreenGui` an instance is under as needing its layout again.
void markUiLayoutDirty(World& world, core::InstanceId id);

} // namespace engine::scene
