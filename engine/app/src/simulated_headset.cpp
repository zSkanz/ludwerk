#include "engine/app/simulated_headset.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <utility>

#include "engine/core/i18n.h"
#include "engine/core/log.h"
#include "engine/core/math.h"

namespace engine::app {

SimulatedHeadset::SimulatedHeadset(rhi::IDevice& device, const SimulatedHeadsetDesc& desc)
    : m_device(device), m_desc(desc), m_width(desc.eyeWidth), m_height(desc.eyeHeight)
{
    makeImages();
}

SimulatedHeadset::~SimulatedHeadset()
{
    releaseImages();
}

void SimulatedHeadset::makeImages()
{
    // sRGB, as the runtimes' images the engine prefers are, so the copy into
    // them is the one a real headset gets.
    for (rhi::TextureHandle& image : m_images) {
        image = m_device.createTexture({
            .format = rhi::TextureFormat::Rgba8UnormSrgb,
            .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
            .width = m_width,
            .height = m_height,
            .debugName = "simulated-eye",
        });
    }
    if (m_desc.depth) {
        for (rhi::TextureHandle& image : m_depthImages) {
            image = m_device.createTexture({
                .format = rhi::TextureFormat::D32Float,
                .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
                .width = m_width,
                .height = m_height,
                .debugName = "simulated-eye-depth",
            });
        }
    }
}

void SimulatedHeadset::releaseImages()
{
    for (std::array<rhi::TextureHandle, 2>* set : {&m_images, &m_depthImages}) {
        for (rhi::TextureHandle& image : *set) {
            if (image.valid())
                m_device.destroy(image);
            image = {};
        }
    }
}

bool SimulatedHeadset::setRenderScale(core::f32 scale)
{
    const core::f32 held = std::clamp(scale, 0.5f, 1.0f);
    const auto scaled = [held](core::u32 size) {
        return std::max(16u, static_cast<core::u32>(static_cast<core::f32>(size) * held + 0.5f));
    };
    const core::u32 width = scaled(m_desc.eyeWidth);
    const core::u32 height = scaled(m_desc.eyeHeight);
    if (width == m_width && height == m_height)
        return true;
    releaseImages();
    m_width = width;
    m_height = height;
    makeImages();
    const std::array<core::I18nArg, 5> args{
        core::I18nArg{"width", static_cast<core::i64>(m_width)},
        core::I18nArg{"height", static_cast<core::i64>(m_height)}, core::I18nArg{"scale", static_cast<core::f64>(held)},
        core::I18nArg{"recommendedWidth", static_cast<core::i64>(m_desc.eyeWidth)},
        core::I18nArg{"recommendedHeight", static_cast<core::i64>(m_desc.eyeHeight)}};
    core::log(core::LogLevel::Info, ENG_TR("rhi.info.xr_eye_scaled"), args);
    return true;
}

void SimulatedHeadset::setHead(const std::array<core::f32, 3>& position,
                               const std::array<core::f32, 4>& orientation) noexcept
{
    m_headPosition = position;
    m_headOrientation = orientation;
}

namespace {

// Where the seated space's origin is in the standing one: the head at rest.
constexpr core::f32 SeatedHeight = 1.6f;

} // namespace

void SimulatedHeadset::setPose(core::u32 which, const std::array<core::f32, 3>& position,
                               const std::array<core::f32, 4>& orientation) noexcept
{
    const std::array<core::f32, 3> standing{position[0], position[1] + (m_seated ? SeatedHeight : 0.0f), position[2]};
    if (which == 0) {
        setHead(standing, orientation);
        return;
    }
    if (which >= 3) {
        // The hand behind the fingertip, the controller put down: every joint
        // at the tip but the thumb's, a hand's width away so it never pinches.
        const core::u32 tracked = which == 3 ? 0u : 1u;
        m_input.grip[tracked].valid = false;
        m_input.aim[tracked].valid = false;
        for (rhi::HeadsetPose& joint : m_input.joints[tracked]) {
            joint.valid = true;
            joint.position = standing;
            joint.orientation = orientation;
        }
        const core::Mat3 turn = core::fromQuaternion(orientation[0], orientation[1], orientation[2], orientation[3]);
        const core::Vec3 aside = turn * core::Vec3{tracked == 0 ? 0.08f : -0.08f, -0.03f, 0.05f};
        rhi::HeadsetPose& thumb = m_input.joints[tracked][rhi::HandJointThumbTip];
        thumb.position = {standing[0] + aside.x, standing[1] + aside.y, standing[2] + aside.z};
        return;
    }
    const core::u32 hand = which == 1 ? 0u : 1u;
    for (rhi::HeadsetPose* pose : {&m_input.grip[hand], &m_input.aim[hand]}) {
        pose->valid = true;
        pose->position = standing;
        pose->orientation = orientation;
    }
}

bool SimulatedHeadset::beginFrame(rhi::HeadsetFrame& frame)
{
    frame = rhi::HeadsetFrame{};
    frame.width = m_width;
    frame.height = m_height;
    frame.format = rhi::TextureFormat::Rgba8UnormSrgb;
    frame.displayTimeNs = static_cast<core::i64>(m_frames) * 13'888'889;
    if (!m_images[0].valid() || !m_images[1].valid())
        return true;
    // Each eye half the separation to its side of the head, along the head's
    // own right, and turned as the head is.
    const core::Mat3 turn =
        core::fromQuaternion(m_headOrientation[0], m_headOrientation[1], m_headOrientation[2], m_headOrientation[3]);
    const core::Vec3 right = turn * core::Vec3{1.0f, 0.0f, 0.0f};
    for (core::u32 index = 0; index < 2; ++index) {
        const core::f32 side = index == 0 ? -0.5f : 0.5f;
        rhi::EyeView& eye = frame.eyes[index];
        eye.position = {m_headPosition[0] + right.x * side * m_desc.eyeSeparation,
                        m_headPosition[1] + right.y * side * m_desc.eyeSeparation - (m_seated ? SeatedHeight : 0.0f),
                        m_headPosition[2] + right.z * side * m_desc.eyeSeparation};
        eye.orientation = m_headOrientation;
        eye.tanLeft = index == 0 ? -m_desc.tanOutward : -m_desc.tanInward;
        eye.tanRight = index == 0 ? m_desc.tanInward : m_desc.tanOutward;
        eye.tanUp = m_desc.tanUp;
        eye.tanDown = m_desc.tanDown;
        frame.color[index] = m_images[index];
        frame.depth[index] = m_depthImages[index];
    }
    if (m_depthImages[0].valid() && m_depthImages[1].valid())
        frame.depthFormat = rhi::TextureFormat::D32Float;
    else
        frame.depth = {};
    frame.draw = true;
    return true;
}

void SimulatedHeadset::readInput(rhi::HeadsetInput& input)
{
    input = m_input;
    input.head.valid = true;
    input.head.position = m_headPosition;
    input.head.orientation = m_headOrientation;
    if (m_seated) {
        input.head.position[1] -= SeatedHeight;
        for (rhi::HeadsetPose& pose : input.grip)
            pose.position[1] -= SeatedHeight;
        for (rhi::HeadsetPose& pose : input.aim)
            pose.position[1] -= SeatedHeight;
        for (std::array<rhi::HeadsetPose, rhi::HandJointCount>& hand : input.joints) {
            for (rhi::HeadsetPose& pose : hand)
                pose.position[1] -= SeatedHeight;
        }
    }
}

bool SimulatedHeadset::takeRoom(rhi::HeadsetRoom& out)
{
    if (!m_roomAsked)
        return false;
    m_roomAsked = false;
    out = rhi::HeadsetRoom{};
    out.known = true;
    // A plane facing along `facing` (its +z), its x across and y along.
    const auto plane = [&out](core::u8 kind, core::Vec3 middle, core::Mat3 turn, core::f32 width, core::f32 height) {
        rhi::HeadsetPlane made;
        made.kind = kind;
        made.pose.valid = true;
        made.pose.position = {middle.x, middle.y, middle.z};
        core::toQuaternion(turn, made.pose.orientation[0], made.pose.orientation[1], made.pose.orientation[2],
                           made.pose.orientation[3]);
        made.width = width;
        made.height = height;
        out.planes.push_back(made);
    };
    constexpr core::f32 HalfPi = 1.57079633f;
    constexpr core::f32 Pi = 3.14159265f;
    // The floor faces up and the ceiling down: a quarter turn about x each way.
    plane(0, {0.0f, 0.0f, 0.0f}, core::fromAxisAngle({1.0f, 0.0f, 0.0f}, -HalfPi), 3.0f, 3.0f);
    plane(1, {0.0f, 2.5f, 0.0f}, core::fromAxisAngle({1.0f, 0.0f, 0.0f}, HalfPi), 3.0f, 3.0f);
    // The walls face in: the north wall's +z is +z, the others turned to it.
    plane(2, {0.0f, 1.25f, -1.5f}, core::fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.0f), 3.0f, 2.5f);
    plane(2, {0.0f, 1.25f, 1.5f}, core::fromAxisAngle({0.0f, 1.0f, 0.0f}, Pi), 3.0f, 2.5f);
    plane(2, {-1.5f, 1.25f, 0.0f}, core::fromAxisAngle({0.0f, 1.0f, 0.0f}, HalfPi), 3.0f, 2.5f);
    plane(2, {1.5f, 1.25f, 0.0f}, core::fromAxisAngle({0.0f, 1.0f, 0.0f}, -HalfPi), 3.0f, 2.5f);
    // The table's top, facing up.
    plane(3, {0.0f, 0.75f, -0.8f}, core::fromAxisAngle({1.0f, 0.0f, 0.0f}, -HalfPi), 1.2f, 0.7f);
    return true;
}

namespace {

// The store's anchors, one a line: the id, then the position and the turn.
[[nodiscard]] std::vector<std::pair<std::string, rhi::HeadsetPose>> readStore(const std::string& path)
{
    std::vector<std::pair<std::string, rhi::HeadsetPose>> kept;
    std::ifstream in(path);
    std::string uuid;
    rhi::HeadsetPose pose;
    while (in >> uuid >> pose.position[0] >> pose.position[1] >> pose.position[2] >> pose.orientation[0] >>
           pose.orientation[1] >> pose.orientation[2] >> pose.orientation[3]) {
        pose.valid = true;
        kept.emplace_back(uuid, pose);
    }
    return kept;
}

} // namespace

core::u32 SimulatedHeadset::createAnchor(const rhi::HeadsetPose& pose)
{
    // An id as a runtime gives one: random, and the same in every session.
    std::random_device random;
    char uuid[33]{};
    (void)std::snprintf(uuid, sizeof(uuid), "%08x%08x%08x%08x", random(), random(), random(), random());
    const core::u64 handle = m_nextAnchor++;
    rhi::HeadsetPose kept = pose;
    kept.valid = true;
    m_anchors.push_back({handle, uuid, kept});
    const core::u32 ask = m_nextAnchorAsk++;
    m_anchorEvents.push_back({rhi::HeadsetAnchorEvent::Kind::Created, ask, handle, uuid, true});
    return ask;
}

core::u32 SimulatedHeadset::saveAnchor(core::u64 anchor)
{
    const core::u32 ask = m_nextAnchorAsk++;
    const auto found =
        std::find_if(m_anchors.begin(), m_anchors.end(), [&](const Anchor& one) { return one.handle == anchor; });
    bool saved = found != m_anchors.end();
    if (saved && m_desc.store.empty()) {
        std::erase_if(m_saved, [&](const Anchor& one) { return one.uuid == found->uuid; });
        m_saved.push_back(*found);
    }
    else if (saved) {
        auto kept = readStore(m_desc.store);
        std::erase_if(kept, [&](const auto& one) { return one.first == found->uuid; });
        kept.emplace_back(found->uuid, found->pose);
        std::ofstream out(m_desc.store, std::ios::trunc);
        for (const auto& [uuid, pose] : kept)
            out << uuid << ' ' << pose.position[0] << ' ' << pose.position[1] << ' ' << pose.position[2] << ' '
                << pose.orientation[0] << ' ' << pose.orientation[1] << ' ' << pose.orientation[2] << ' '
                << pose.orientation[3] << '\n';
        saved = static_cast<bool>(out);
    }
    m_anchorEvents.push_back({rhi::HeadsetAnchorEvent::Kind::Saved, ask, 0, {}, saved});
    return ask;
}

core::u32 SimulatedHeadset::loadAnchors()
{
    const core::u32 ask = m_nextAnchorAsk++;
    std::vector<std::pair<std::string, rhi::HeadsetPose>> kept;
    if (m_desc.store.empty()) {
        for (const Anchor& one : m_saved)
            kept.emplace_back(one.uuid, one.pose);
    }
    else {
        kept = readStore(m_desc.store);
    }
    for (const auto& [uuid, pose] : kept) {
        const core::u64 handle = m_nextAnchor++;
        m_anchors.push_back({handle, uuid, pose});
        m_anchorEvents.push_back({rhi::HeadsetAnchorEvent::Kind::Found, ask, handle, uuid, true});
    }
    m_anchorEvents.push_back({rhi::HeadsetAnchorEvent::Kind::Done, ask, 0, {}, true});
    return ask;
}

void SimulatedHeadset::destroyAnchor(core::u64 anchor)
{
    std::erase_if(m_anchors, [&](const Anchor& one) { return one.handle == anchor; });
}

rhi::HeadsetPose SimulatedHeadset::locateAnchor(core::u64 anchor)
{
    const auto found =
        std::find_if(m_anchors.begin(), m_anchors.end(), [&](const Anchor& one) { return one.handle == anchor; });
    return found != m_anchors.end() ? found->pose : rhi::HeadsetPose{};
}

void SimulatedHeadset::takeAnchorEvents(std::vector<rhi::HeadsetAnchorEvent>& out)
{
    for (rhi::HeadsetAnchorEvent& one : m_anchorEvents)
        out.push_back(std::move(one));
    m_anchorEvents.clear();
}

void SimulatedHeadset::vibrate(core::u32 hand, core::f32 amplitude, core::f32 seconds)
{
    (void)seconds;
    if (hand < 2)
        m_vibration[hand] = amplitude;
}

} // namespace engine::app
