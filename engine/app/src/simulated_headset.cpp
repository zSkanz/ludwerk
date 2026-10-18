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
}

SimulatedHeadset::~SimulatedHeadset()
{
    for (const rhi::TextureHandle image : m_images) {
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
                        m_headPosition[1] + right.y * side * m_desc.eyeSeparation,
                        m_headPosition[2] + right.z * side * m_desc.eyeSeparation};
        eye.orientation = m_headOrientation;
        eye.tanLeft = index == 0 ? -m_desc.tanOutward : -m_desc.tanInward;
        eye.tanRight = index == 0 ? m_desc.tanInward : m_desc.tanOutward;
        eye.tanUp = m_desc.tanUp;
        eye.tanDown = m_desc.tanDown;
        frame.color[index] = m_images[index];
    }
    frame.draw = true;
    return true;
}

} // namespace engine::app
