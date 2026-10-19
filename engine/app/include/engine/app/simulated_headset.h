// **A headset that is the engine's own** (ADR 0248 §4, `--xr=simulated`): two
// eyes at a set separation and field of view, its "swapchains" ordinary
// textures, a head that stands where it is put. It needs no OpenXR runtime and
// no window, so every XR test runs where the other tests do -- headless, in
// the container on lavapipe. Not a stand-in for the device: the rate held on a
// real headset and what its lenses show are the maintainer's to see.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

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
    // **Each eye's depth image** (ADR 0244 §5), as a runtime that takes
    // depth gives one: what `--xr-depth` turns off.
    bool depth = true;
    // **Where saved anchors are kept** (`--xr-simulated-store`): a file a
    // second session reads them back from. Empty keeps them for this session.
    std::string store;
};

class SimulatedHeadset final : public rhi::IHeadset
{
public:
    SimulatedHeadset(rhi::IDevice& device, const SimulatedHeadsetDesc& desc);
    ~SimulatedHeadset() override;

    [[nodiscard]] std::string_view runtimeName() const noexcept override { return "simulated"; }
    [[nodiscard]] std::string_view systemName() const noexcept override { return "simulated"; }
    [[nodiscard]] core::u32 eyeWidth() const noexcept override { return m_width; }
    [[nodiscard]] core::u32 eyeHeight() const noexcept override { return m_height; }
    // The images remade at `scale` of the desc's size, as a runtime's are.
    [[nodiscard]] bool setRenderScale(core::f32 scale) override;
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
    // and tracked from then on -- 3 and 4 the left and right index
    // fingertips, which put that hand's controller down and track the hand:
    // an open hand, its finger along the pose's look vector.
    void setPose(core::u32 which, const std::array<core::f32, 3>& position,
                 const std::array<core::f32, 4>& orientation) noexcept;
    // **A control held at a value** (`XRService:SimulateInput`), by its
    // place among the `XR` key codes.
    void setKey(core::usize index, core::f32 value) noexcept
    {
        if (index < m_input.keys.size())
            m_input.keys[index] = value;
    }
    // What each controller was last asked to vibrate at, for a test to read.
    [[nodiscard]] core::f32 vibration(core::u32 hand) const noexcept { return m_vibration[hand & 1u]; }
    // The eyes' images as the last frame left them, for a test to read back.
    [[nodiscard]] rhi::TextureHandle image(core::u32 eye) const noexcept { return m_images[eye]; }
    [[nodiscard]] rhi::TextureHandle depthImage(core::u32 eye) const noexcept { return m_depthImages[eye]; }
    // **A recorded room** (ADR 0247 §2), as the Quest's scene gives one: a
    // floor three metres by three, the ceiling at 2.5, four walls, and a table
    // whose top is 0.75 up -- answered at the frame after it is asked for.
    void requestRoom() override { m_roomAsked = true; }
    // **Anchors** (ADR 0247 §3), as a runtime keeps them: each a pose in the
    // space the headset gives, answered at the next frame; those saved are
    // written to the store.
    [[nodiscard]] core::u32 createAnchor(const rhi::HeadsetPose& pose) override;
    [[nodiscard]] core::u32 saveAnchor(core::u64 anchor) override;
    [[nodiscard]] core::u32 loadAnchors() override;
    void destroyAnchor(core::u64 anchor) override;
    [[nodiscard]] rhi::HeadsetPose locateAnchor(core::u64 anchor) override;
    void takeAnchorEvents(std::vector<rhi::HeadsetAnchorEvent>& out) override;
    [[nodiscard]] bool takeRoom(rhi::HeadsetRoom& out) override;
    // Its frames at 72 Hz, each on time: the shape of the numbers a runtime's
    // timing has, for the log a test reads.
    [[nodiscard]] rhi::HeadsetTiming takeTiming() override
    {
        rhi::HeadsetTiming taken;
        taken.displayPeriodNs = 13'888'889;
        taken.frames = static_cast<core::u32>(m_frames - m_framesTaken);
        // From the first frame's wait to the last's, as a runtime counts it.
        taken.intervals = taken.frames > 0 ? taken.frames - 1 : 0;
        taken.spanNs = static_cast<core::i64>(taken.intervals) * taken.displayPeriodNs;
        m_framesTaken = m_frames;
        return taken;
    }
    // The room seen through: always offered, as a runtime with the alpha
    // blend does; whether it is on, for a test to read.
    [[nodiscard]] bool supportsPassthrough() const noexcept override { return true; }
    void setPassthrough(bool on) override { m_passthrough = on; }
    void setRefreshRate(core::f32 hertz) override { m_refreshRate = hertz; }
    // The rate a script last asked for, for a test to read.
    [[nodiscard]] core::f32 refreshRate() const noexcept { return m_refreshRate; }
    [[nodiscard]] bool passthrough() const noexcept { return m_passthrough; }
    void setDepthRange(core::f32 nearZ, core::f32 farZ) override
    {
        m_near = nearZ;
        m_far = farZ;
    }
    // What the frames' depth meant, as the engine said, for a test to read.
    [[nodiscard]] core::f32 depthNear() const noexcept { return m_near; }
    [[nodiscard]] core::f32 depthFar() const noexcept { return m_far; }
    [[nodiscard]] core::u64 framesEnded() const noexcept { return m_frames; }

private:
    // The eyes' images, colour and depth, at `m_width` by `m_height`.
    void makeImages();
    void releaseImages();

    rhi::IDevice& m_device;
    SimulatedHeadsetDesc m_desc;
    core::u32 m_width = 0;
    core::u32 m_height = 0;
    std::array<rhi::TextureHandle, 2> m_images{};
    std::array<rhi::TextureHandle, 2> m_depthImages{};
    bool m_passthrough = false;
    core::f32 m_refreshRate = 0.0f;
    core::u64 m_framesTaken = 0;
    bool m_roomAsked = false;
    struct Anchor
    {
        core::u64 handle = 0;
        std::string uuid;
        rhi::HeadsetPose pose;
    };
    std::vector<Anchor> m_anchors;
    // Saved, where no store is named.
    std::vector<Anchor> m_saved;
    std::vector<rhi::HeadsetAnchorEvent> m_anchorEvents;
    core::u32 m_nextAnchorAsk = 1;
    core::u64 m_nextAnchor = 1;
    core::f32 m_near = 0.0f;
    core::f32 m_far = 0.0f;
    std::array<core::f32, 3> m_headPosition{0.0f, 1.6f, 0.0f};
    std::array<core::f32, 4> m_headOrientation{0.0f, 0.0f, 0.0f, 1.0f};
    core::u64 m_frames = 0;
    rhi::HeadsetInput m_input{};
    std::array<core::f32, 2> m_vibration{};
    bool m_seated = false;
};

} // namespace engine::app
