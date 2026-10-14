#include "engine/scene/ui_pages.h"

#include <algorithm>
#include <cmath>

#include "engine/scene/world.h"

namespace engine::scene {
namespace {

using core::f32;
using core::i32;
using core::usize;

void pageEvent(World& world, core::InstanceId layout, const char* event, core::InstanceId page)
{
    if (!world.alive(layout) || !world.alive(page))
        return;
    world.changes().push(Change{ChangeKind::InstanceEvent, layout, page, world.atoms().intern(event)});
}

} // namespace

void uiChildrenInOrder(const World& world, core::InstanceId parent, core::i32 sortOrder,
                       std::vector<core::InstanceId>& out)
{
    out.clear();
    for (core::InstanceId child = world.firstChild(parent); child.valid(); child = world.nextSibling(child)) {
        if (const UIObjectComponent* object = world.uiObjects().find(child); object != nullptr && object->visible)
            out.push_back(child);
    }
    if (sortOrder == kUiSortByDocument)
        return;

    if (sortOrder == kUiSortByName) {
        std::stable_sort(out.begin(), out.end(), [&world](core::InstanceId a, core::InstanceId b) {
            return world.atoms().text(world.name(a)) < world.atoms().text(world.name(b));
        });
        return;
    }
    std::stable_sort(out.begin(), out.end(), [&world](core::InstanceId a, core::InstanceId b) {
        return world.uiObjects().find(a)->layoutOrder < world.uiObjects().find(b)->layoutOrder;
    });
}

void pagesOf(const World& world, core::InstanceId layout, std::vector<core::InstanceId>& out)
{
    out.clear();
    const UIPageLayoutComponent* page = world.uiPageLayouts().find(layout);
    if (page == nullptr)
        return;
    uiChildrenInOrder(world, world.parentOf(layout), page->sortOrder, out);
}

void markUiLayoutDirty(World& world, core::InstanceId id)
{
    for (core::InstanceId current = id; current.valid(); current = world.parentOf(current)) {
        if (ScreenGuiComponent* screen = world.screenGuis().find(current); screen != nullptr) {
            screen->layoutDirty = true;
            return;
        }
    }
}

bool turnPage(World& world, core::InstanceId layout, core::i32 index, core::i32 direction)
{
    UIPageLayoutComponent* page = world.uiPageLayouts().find(layout);
    if (page == nullptr)
        return false;
    std::vector<core::InstanceId> pages;
    pagesOf(world, layout, pages);
    const i32 count = static_cast<i32>(pages.size());
    if (count == 0)
        return false;

    // Past an end: round it when the pages go round, and the end otherwise.
    const i32 target = page->circular ? ((index % count) + count) % count : std::clamp(index, 0, count - 1);

    const core::InstanceId was = page->currentPage;
    const core::InstanceId now = pages[static_cast<usize>(target)];
    if (was == now && page->index == target)
        return false;

    page->index = target;
    page->currentPage = now;
    pageEvent(world, layout, "PageLeave", was);
    pageEvent(world, layout, "PageEnter", now);
    world.changes().push(Change{ChangeKind::PropertyChanged, layout, {}, world.atoms().intern("CurrentPage")});

    if (page->animated && page->tweenTime > 0.0f) {
        // **Which way round**: the way that was asked for, so `Next` at the
        // last page slides on to the first rather than back through them all;
        // with no way asked, the shorter.
        f32 to = static_cast<f32>(target);
        if (page->circular && count > 1) {
            const f32 lap = static_cast<f32>(count);
            if (direction > 0) {
                while (to < page->position)
                    to += lap;
            }
            else if (direction < 0) {
                while (to > page->position)
                    to -= lap;
            }
            else if (std::fabs(to + lap - page->position) < std::fabs(to - page->position)) {
                to += lap;
            }
            else if (std::fabs(to - lap - page->position) < std::fabs(to - page->position)) {
                to -= lap;
            }
        }
        page->slideFrom = page->position;
        page->slideTo = to;
        page->slideElapsed = 0.0f;
        page->sliding = true;
    }
    else {
        page->position = static_cast<f32>(target);
        page->sliding = false;
        pageEvent(world, layout, "Stopped", now);
    }
    markUiLayoutDirty(world, layout);
    return true;
}

bool stepPage(World& world, core::InstanceId layout, core::i32 step)
{
    const UIPageLayoutComponent* page = world.uiPageLayouts().find(layout);
    if (page == nullptr || step == 0)
        return false;
    // From the page that is there now, which the layout may not have seen yet.
    std::vector<core::InstanceId> pages;
    pagesOf(world, layout, pages);
    const auto here = std::find(pages.begin(), pages.end(), page->currentPage);
    const i32 index = here != pages.end() ? static_cast<i32>(here - pages.begin()) : page->index;
    return turnPage(world, layout, index + step, step);
}

} // namespace engine::scene
