// **A headset that is the engine's own** (ADR 0248 §4, `--xr=simulated`): two
// eyes at a set separation and field of view, its "swapchains" ordinary
// textures, a head that stands where it is put. It needs no OpenXR runtime and
// no window, so every XR test runs where the other tests do -- headless, in
// the container on lavapipe. Not a stand-in for the device: the rate held on a
// real headset and what its lenses show are the maintainer's to see.
#pragma once

#include <array>
#include <memory>

#include "engine/rhi/device.h"
#include "engine/rhi/xr.h"

namespace engine::app {

struct SimulatedHeadsetDesc
{
    core::u32 eyeWidth = 1024;
    core::u32 eyeHeight = 1024;
    // Between the eyes, in metres: an adult's average.
    core::f32 eyeSeparation = 0.063f;
    // Each eye's edges as tangents, an eye seeing further outward than toward
    // the nose, as a headset's lenses do.
    core::f32 tanOutward = 1.19f;
    core::f32 tanInward = 0.96f;
    core::f32 tanUp = 1.0f;
    core::f32 tanDown = -1.15f;
};

class SimulatedHeadset final : public rhi::IHeadset
{
public:
    SimulatedHeadset(rhi::IDevice& device, const SimulatedHeadsetDesc& desc);
    ~SimulatedHeadset() override;

    [[nodiscard]] std::string_view runtimeName() const noexcept override { return "simulated"; }
    [[nodiscard]] std::string_view systemName() const noexcept override { return "simulated"; }
    [[nodiscard]] core::u32 eyeWidth() const noexcept override { return m_desc.eyeWidth; }
    [[nodiscard]] core::u32 eyeHeight() const noexcept override { return m_desc.eyeHeight; }
    void poll() override {}
    [[nodiscard]] rhi::HeadsetState state() const noexcept override { return rhi::HeadsetState::Running; }
    [[nodiscard]] bool beginFrame(rhi::HeadsetFrame& frame) override;
    void endFrame() override { m_frames += 1; }
    // The head where it is put, and the hands and the controllers as set.
    void readInput(rhi::HeadsetInput& input) override;
    void vibrate(core::u32 hand, core::f32 amplitude, core::f32 seconds) override;

    // **Where the head is**, in the play space: a position in metres and a
    // unit quaternion (x, y, z, w). Standing at 1.6 m, looking ahead, until set.
    void setHead(const std::array<core::f32, 3>& position, const std::array<core::f32, 4>& orientation) noexcept;
    // **The hands and the controllers**, as a test or a recorded path puts
    // them: `grip`, `aim` and `keys` are read back as they are; `head` is
    // not, being `setHead`'s.
    void setInput(const rhi::HeadsetInput& input) noexcept { m_input = input; }
    // **Seated or standing** (ADR 0245 §1): seated, every pose is from where
    // the head stands at rest -- 1.6 m over the floor -- as OpenXR's local
    // space is from the head's start.
    void setSpace(rhi::HeadsetSpace space) override { m_seated = space == rhi::HeadsetSpace::Seated; }
    // A room three metres by three.
    [[nodiscard]] std::optional<std::array<core::f32, 2>> playArea() const override
    {
        return std::array<core::f32, 2>{3.0f, 3.0f};
    }
    // **A pose put in the space chosen now** (`XRService:SimulatePose`):
    // 0 the head, 1 the left hand, 2 the right -- a hand's grip and aim both,
    // and tracked from then on.
    void setPose(core::u32 which, const std::array<core::f32, 3>& position,
                 const std::array<core::f32, 4>& orientation) noexcept;
    // What each controller was last asked to vibrate at, for a test to read.
    [[nodiscard]] core::f32 vibration(core::u32 hand) const noexcept { return m_vibration[hand & 1u]; }
    // The eyes' images as the last frame left them, for a test to read back.
    [[nodiscard]] rhi::TextureHandle image(core::u32 eye) const noexcept { return m_images[eye]; }
    [[nodiscard]] core::u64 framesEnded() const noexcept { return m_frames; }

private:
    rhi::IDevice& m_device;
    SimulatedHeadsetDesc m_desc;
    std::array<rhi::TextureHandle, 2> m_images{};
    std::array<core::f32, 3> m_headPosition{0.0f, 1.6f, 0.0f};
    std::array<core::f32, 4> m_headOrientation{0.0f, 0.0f, 0.0f, 1.0f};
    core::u64 m_frames = 0;
    rhi::HeadsetInput m_input{};
    std::array<core::f32, 2> m_vibration{};
    bool m_seated = false;
};

} // namespace engine::app
