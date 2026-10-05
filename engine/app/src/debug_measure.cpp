#include "engine/app/debug_measure.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "engine/core/i18n.h"
#include "engine/core/log.h"

namespace engine::app {

namespace {

using core::f64;
using core::u32;
using core::u64;

struct HideName
{
    std::string_view name;
    bool DebugHide::*field;
};

constexpr std::array<HideName, 14> kHideNames{{
    {"parts", &DebugHide::parts},
    {"skinned", &DebugHide::skinned},
    {"terrain", &DebugHide::terrain},
    {"voxels", &DebugHide::voxels},
    {"foliage", &DebugHide::foliage},
    {"transparent", &DebugHide::transparent},
    {"particles", &DebugHide::particles},
    {"ribbons", &DebugHide::ribbons},
    {"decals", &DebugHide::decals},
    {"sprites", &DebugHide::sprites},
    {"world_ui", &DebugHide::worldUi},
    {"ui", &DebugHide::ui},
    {"lights", &DebugHide::lights},
    {"highlights", &DebugHide::highlights},
}};

constexpr std::array<std::string_view, 14> kNames = [] {
    std::array<std::string_view, 14> names{};
    for (std::size_t index = 0; index < names.size(); ++index)
        names[index] = kHideNames[index].name;
    return names;
}();

struct SkipName
{
    std::string_view name;
    u32 bit;
};

constexpr std::array<SkipName, 11> kSkipNames{{
    {"sun", render::MeasureSkip::Sun},
    {"shadow", render::MeasureSkip::Shadow},
    {"contact", render::MeasureSkip::Contact},
    {"lights", render::MeasureSkip::Lights},
    {"environment", render::MeasureSkip::Environment},
    {"ambient", render::MeasureSkip::Ambient},
    {"occlusion", render::MeasureSkip::Occlusion},
    {"fog", render::MeasureSkip::Fog},
    {"normal_map", render::MeasureSkip::NormalMap},
    {"material_maps", render::MeasureSkip::MaterialMaps},
    {"unlit", render::MeasureSkip::Unlit},
}};

constexpr std::array<std::string_view, 11> kSkips = [] {
    std::array<std::string_view, 11> names{};
    for (std::size_t index = 0; index < names.size(); ++index)
        names[index] = kSkipNames[index].name;
    return names;
}();

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string fixed(f64 value, int decimals)
{
    std::array<char, 48> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
    return written > 0 ? std::string(buffer.data(), static_cast<std::size_t>(written)) : std::string{};
}

} // namespace

std::span<const std::string_view> debugHideNames() noexcept
{
    return kNames;
}

DebugHide parseDebugHide(std::string_view list, std::vector<std::string>* unknown)
{
    DebugHide hide;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view name = trimmed(list.substr(0, comma));
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        if (name.empty())
            continue;
        const auto found = std::find_if(kHideNames.begin(), kHideNames.end(),
                                        [name](const HideName& entry) { return entry.name == name; });
        if (found != kHideNames.end())
            hide.*(found->field) = true;
        else if (unknown != nullptr)
            unknown->emplace_back(name);
    }
    return hide;
}

std::string debugHideText(const DebugHide& hide)
{
    std::string text;
    for (const HideName& entry : kHideNames) {
        if (!(hide.*(entry.field)))
            continue;
        if (!text.empty())
            text += ',';
        text += entry.name;
    }
    return text;
}

void applyDebugHide(render::RenderWorld& world, const DebugHide& hide)
{
    if (!hide.any())
        return;
    if (hide.parts || hide.skinned || hide.terrain || hide.voxels || hide.transparent) {
        // In place and in order: the list is sorted, and what is left is drawn
        // as it would have been.
        std::erase_if(world.draws, [&hide](const render::DrawItem& draw) {
            if (hide.transparent && draw.transparent)
                return true;
            if (draw.terrain)
                return hide.terrain;
            if (draw.voxelBlock)
                return hide.voxels;
            if (draw.boneCount != 0)
                return hide.skinned;
            return hide.parts;
        });
    }
    if (hide.terrain)
        world.terrains.clear();
    if (hide.foliage) {
        world.foliageRuns.clear();
        world.foliageBuckets.clear();
    }
    if (hide.particles) {
        world.particles.clear();
        world.gpuEmitters.clear();
    }
    if (hide.ribbons) {
        world.ribbonVertices.clear();
        world.ribbonRuns.clear();
    }
    if (hide.decals)
        world.decals.clear();
    if (hide.sprites)
        world.sprites.clear();
    if (hide.worldUi) {
        world.worldUiVertices.clear();
        world.worldUiRuns.clear();
    }
    if (hide.lights)
        world.lights.clear();
    if (hide.highlights)
        world.highlights.clear();
}

std::span<const std::string_view> debugSkipNames() noexcept
{
    return kSkips;
}

u32 parseDebugSkip(std::string_view list, std::vector<std::string>* unknown)
{
    u32 skip = 0;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view name = trimmed(list.substr(0, comma));
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
        if (name.empty())
            continue;
        const auto found = std::find_if(kSkipNames.begin(), kSkipNames.end(),
                                        [name](const SkipName& entry) { return entry.name == name; });
        if (found != kSkipNames.end())
            skip |= found->bit;
        else if (unknown != nullptr)
            unknown->emplace_back(name);
    }
    return skip;
}

std::string debugSkipText(u32 skip)
{
    std::string text;
    for (const SkipName& entry : kSkipNames) {
        if ((skip & entry.bit) == 0)
            continue;
        if (!text.empty())
            text += ',';
        text += entry.name;
    }
    return text;
}

std::string debugKeysInForce(bool gpuPassTimes, const DebugHide& hide, u32 shadowTaps, bool logUiTouches, u32 skip)
{
    std::string keys;
    const auto add = [&keys](std::string_view key) {
        if (!keys.empty())
            keys += ", ";
        keys += key;
    };
    if (gpuPassTimes)
        add("gpu_pass_times");
    if (hide.any())
        add("hide=" + debugHideText(hide));
    if (skip != 0)
        add("skip=" + debugSkipText(skip));
    if (shadowTaps != 0)
        add("shadow_taps=" + std::to_string(shadowTaps));
    if (logUiTouches)
        add("log_ui_touches");
    return keys;
}

std::string wholeName(const scene::World& world, core::InstanceId id)
{
    std::vector<std::string_view> names;
    for (core::InstanceId at = id; at.valid(); at = world.parentOf(at))
        names.push_back(world.atoms().text(world.name(at)));
    std::string path;
    for (auto name = names.rbegin(); name != names.rend(); ++name) {
        if (!path.empty())
            path += '.';
        path += *name;
    }
    return path;
}

void logUiTouch(const scene::World& world, core::InstanceId under, core::i64 finger, core::Vec2 at)
{
    using core::I18nArg;
    const auto whole = [](core::f32 value) { return std::round(static_cast<f64>(value)); };
    if (!under.valid()) {
        const std::array<I18nArg, 3> free{I18nArg{"finger", finger}, I18nArg{"x", whole(at.x)},
                                          I18nArg{"y", whole(at.y)}};
        core::log(core::LogLevel::Info, ENG_TR("engine.ui.info.finger_free"), free);
        return;
    }
    const scene::UIObjectComponent* object = world.uiObjects().find(under);
    const core::Vec2 corner = object != nullptr ? object->absolutePosition : core::Vec2{};
    const core::Vec2 size = object != nullptr ? object->absoluteSize : core::Vec2{};
    const std::string element = wholeName(world, under);
    const std::array<I18nArg, 8> taken{
        I18nArg{"finger", finger},       I18nArg{"x", whole(at.x)},        I18nArg{"y", whole(at.y)},
        I18nArg{"element", element},     I18nArg{"left", whole(corner.x)}, I18nArg{"top", whole(corner.y)},
        I18nArg{"width", whole(size.x)}, I18nArg{"height", whole(size.y)},
    };
    core::log(core::LogLevel::Info, ENG_TR("engine.ui.info.finger_taken"), taken);
}

void PassTimeLedger::add(std::span<const rhi::PassTime> frame)
{
    if (frame.empty())
        return;
    ++frames_;
    for (const rhi::PassTime& time : frame) {
        auto found = std::find_if(totals_.begin(), totals_.end(),
                                  [&time](const Total& total) { return total.name == time.name; });
        if (found == totals_.end())
            found = totals_.insert(totals_.end(), Total{std::string(time.name), 0.0, 0});
        found->milliseconds += time.milliseconds;
        found->submits += time.submits;
    }
}

void PassTimeLedger::clear() noexcept
{
    totals_.clear();
    frames_ = 0;
}

PassTimeLedger::Line PassTimeLedger::line() const
{
    Line out;
    if (frames_ == 0)
        return out;
    const auto frames = static_cast<f64>(frames_);
    std::vector<const Total*> order;
    order.reserve(totals_.size());
    for (const Total& total : totals_) {
        // The stop that cleared one pixel is the price of a stop, not a pass.
        if (total.name == "floor") {
            out.floor = total.submits != 0 ? total.milliseconds / static_cast<f64>(total.submits) : 0.0;
            continue;
        }
        order.push_back(&total);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const Total* left, const Total* right) { return left->milliseconds > right->milliseconds; });
    for (const Total* total : order) {
        const f64 mean = total->milliseconds / frames;
        if (!out.passes.empty())
            out.passes += ", ";
        out.passes += total->name;
        out.passes += ' ';
        out.passes += fixed(mean, 2);
        out.total += mean;
        out.submits += static_cast<f64>(total->submits) / frames;
    }
    return out;
}

} // namespace engine::app
