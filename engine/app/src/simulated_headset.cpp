#include "engine/app/simulated_headset.h"

#include "engine/core/math.h"

namespace engine::app {

SimulatedHeadset::SimulatedHeadset(rhi::IDevice& device, const SimulatedHeadsetDesc& desc)
    : m_device(device), m_desc(desc)
{
    // sRGB, as the runtimes' images the engine prefers are, so the copy into
    // them is the one a real headset gets.
    for (rhi::TextureHandle& image : m_images) {
        image = m_device.createTexture({
            .format = rhi::TextureFormat::Rgba8UnormSrgb,
            .usage = rhi::TextureUsage::ColorTarget | rhi::TextureUsage::Sampled,
            .width = m_desc.eyeWidth,
            .height = m_desc.eyeHeight,
            .debugName = "simulated-eye",
        });
    }
    if (desc.depth) {
        for (rhi::TextureHandle& image : m_depthImages) {
            image = m_device.createTexture({
                .format = rhi::TextureFormat::D32Float,
                .usage = rhi::TextureUsage::DepthStencilTarget | rhi::TextureUsage::Sampled,
                .width = m_desc.eyeWidth,
                .height = m_desc.eyeHeight,
                .debugName = "simulated-eye-depth",
            });
        }
    }
}

SimulatedHeadset::~SimulatedHeadset()
{
    for (const rhi::TextureHandle image : m_images) {
        if (image.valid())
            m_device.destroy(image);
    }
    for (const rhi::TextureHandle image : m_depthImages) {
        if (image.valid())
            m_device.destroy(image);
    }
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
    frame.width = m_desc.eyeWidth;
    frame.height = m_desc.eyeHeight;
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

void SimulatedHeadset::vibrate(core::u32 hand, core::f32 amplitude, core::f32 seconds)
{
    (void)seconds;
    if (hand < 2)
        m_vibration[hand] = amplitude;
}

} // namespace engine::app
