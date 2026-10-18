// A headset, as the RHI hands it to the renderer (ADR 0244 §7).
//
// **The RHI's interface was frozen (ADR 0037); this adds to it and changes
// nothing there.** A device may be made for a headset (`DeviceDesc::xr`), and
// then `IDevice::headset()` answers one: each frame it says whether to draw,
// for when, from where each eye looks, and which texture each eye draws into
// -- an image of the runtime's own swapchain, as a `TextureHandle` like any.
// The renderer records into them in the frame's command list, the frame is
// submitted, and `endFrame` hands the images back and shows them.
//
// No OpenXR type crosses this line (R17): a pose is a position and a
// quaternion, a field of view four tangents, an instant nanoseconds.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "engine/core/types.h"
#include "engine/rhi/types.h"

namespace engine::rhi {

// **What asks for a headset when the device is made.** Nothing is asked when
// `enabled` is false, and a device made with it true where there is no
// headset is still a device -- `headset()` answers null and the game draws on
// the screen (ADR 0244 §6).
struct XrRequest
{
    bool enabled = false;
    // The application's name and version, as the runtime lists it.
    std::string application = {};
    u32 applicationVersion = 0;
    // `[xr] hands` (ADR 0245 §4): the hands themselves, joint by joint,
    // where the runtime tracks them.
    bool hands = true;
    // `[xr] depth` (ADR 0244 §5): each eye's depth handed to the runtime with
    // its colour, where the runtime takes it, for its reprojection.
    bool depth = true;
};

// One eye's view this frame: where it is and how it is turned, in the play
// space's metres (y up, the runtime's floor at 0 where it knows one), and the
// edges of what it sees as tangents of their angles from straight ahead --
// left and down negative. An eye's frustum is not symmetric.
struct EyeView
{
    std::array<f32, 3> position{0.0f, 0.0f, 0.0f};
    // x, y, z, w.
    std::array<f32, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
    f32 tanLeft = -1.0f;
    f32 tanRight = 1.0f;
    f32 tanDown = -1.0f;
    f32 tanUp = 1.0f;
};

// Where the headset's session stands: `Running` is the only state frames are
// drawn in; `Ended` is for good, and the game goes on on the screen.
// **Which space the poses are in** (ADR 0245 §1): the floor where the player
// stood (`Standing`, OpenXR's stage) or the head where the session began
// (`Seated`, OpenXR's local). A runtime that knows no floor gives the
// head's start for both.
enum class HeadsetSpace : u8
{
    Standing,
    Seated,
};

enum class HeadsetState : u8
{
    Idle,
    Running,
    Paused,
    Ended,
};

// **One frame of the headset.** `draw` false is a frame the runtime does not
// want drawn -- the headset is off the head, the system menu is up -- and it
// is still ended, with nothing in it.
struct HeadsetFrame
{
    bool draw = false;
    // When the runtime will show it, in nanoseconds of the runtime's clock.
    i64 displayTimeNs = 0;
    std::array<EyeView, 2> eyes{};
    std::array<TextureHandle, 2> color{};
    u32 width = 0;
    u32 height = 0;
    TextureFormat format = TextureFormat::Undefined;
    // **Each eye's depth image**, the size of its colour, where the runtime
    // takes depth (ADR 0244 §5); none otherwise. A frame that has them has
    // each written before it ends, as `setDepthRange` says it is.
    std::array<TextureHandle, 2> depth{};
    TextureFormat depthFormat = TextureFormat::Undefined;
};

// A place and a turn in the play space, and whether the runtime knows it.
struct HeadsetPose
{
    bool valid = false;
    std::array<f32, 3> position{0.0f, 0.0f, 0.0f};
    // x, y, z, w.
    std::array<f32, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

// **The number of controller values a frame reads** (ADR 0245): one for each
// `Enum.KeyCode` whose name begins with `XR`, in the enum's order -- A, B, X,
// Y, the menu, the two triggers, the two grips, the four stick axes, the two
// sticks as one (unused here), the two sticks pressed in, the two pinches.
inline constexpr usize HeadsetKeyCount = 19;
// Where the pinches are among them.
inline constexpr usize HeadsetPinchLeft = 17;

// **A hand's joints** (ADR 0245 §4), in OpenXR's order: the palm, the wrist,
// then each finger from the thumb out, from its root to its tip.
inline constexpr usize HandJointCount = 26;
inline constexpr usize HandJointPalm = 0;
inline constexpr usize HandJointThumbTip = 5;
inline constexpr usize HandJointIndexTip = 10;

// **What the player is doing this frame**: the head, each hand where it holds
// its controller (`grip`) and where it points (`aim`), and the controllers'
// buttons, triggers and sticks. Left is 0 and right 1.
struct HeadsetInput
{
    HeadsetPose head;
    std::array<HeadsetPose, 2> grip{};
    std::array<HeadsetPose, 2> aim{};
    std::array<f32, HeadsetKeyCount> keys{};
    // Each hand's joints where it is tracked as a hand -- no controller in
    // it -- and every one not valid where it is not.
    std::array<std::array<HeadsetPose, HandJointCount>, 2> joints{};
};

class IHeadset
{
public:
    virtual ~IHeadset() = default;
    IHeadset() = default;
    IHeadset(const IHeadset&) = delete;
    IHeadset& operator=(const IHeadset&) = delete;

    // The runtime's name ("Oculus", "SteamVR/OpenXR", "simulated") and the
    // headset's, for the log and the profiler.
    [[nodiscard]] virtual std::string_view runtimeName() const noexcept = 0;
    [[nodiscard]] virtual std::string_view systemName() const noexcept = 0;
    // The size each eye is drawn at, as the runtime recommends it.
    [[nodiscard]] virtual u32 eyeWidth() const noexcept = 0;
    [[nodiscard]] virtual u32 eyeHeight() const noexcept = 0;

    // Reads what the runtime said since the last call -- a session ready to
    // begin, asked to stop, lost -- and acts on it. Once a frame, before
    // `beginFrame`.
    virtual void poll() = 0;
    [[nodiscard]] virtual HeadsetState state() const noexcept = 0;

    // **Waits for the runtime's moment to draw**, at the headset's rate, and
    // begins the frame: the eyes' views for when it will be shown, and the
    // eyes' images to draw into. False when no frame was begun (not running,
    // or the runtime refused); `endFrame` is then not called. After
    // `IDevice::beginFrame`, since the images are recorded into its list.
    [[nodiscard]] virtual bool beginFrame(HeadsetFrame& frame) = 0;
    // **Hands the images back and shows them**, after the frame's command
    // list was submitted. Every begun frame is ended, drawn or not.
    virtual void endFrame() = 0;

    // **The head, the hands and the controllers** (ADR 0245), for the frame
    // begun last -- read at the time it will be shown. Nothing known reads
    // as poses not valid and every key at rest.
    virtual void readInput(HeadsetInput& input) { input = HeadsetInput{}; }
    // **A controller's vibration** (ADR 0245 §3): `hand` 0 left, 1 right; an
    // amplitude 0 to 1 for `seconds`, replacing what it was doing.
    virtual void vibrate(u32 hand, f32 amplitude, f32 seconds)
    {
        (void)hand;
        (void)amplitude;
        (void)seconds;
    }
    // **What the depth images hold**: depth 0 at `nearZ` metres and 1 at
    // `farZ`, as the eyes were drawn. Before `endFrame`.
    virtual void setDepthRange(f32 nearZ, f32 farZ)
    {
        (void)nearZ;
        (void)farZ;
    }
    // **The space the poses and the eyes are in from the next frame**
    // (ADR 0245 §1). Standing until asked.
    virtual void setSpace(HeadsetSpace space) { (void)space; }
    // **The floor's play area**: the width and depth, in metres, of the
    // rectangle the player set up around the standing space's origin, or
    // nothing where the runtime knows none.
    [[nodiscard]] virtual std::optional<std::array<f32, 2>> playArea() const { return std::nullopt; }
};

} // namespace engine::rhi
